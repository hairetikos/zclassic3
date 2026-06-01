// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2016-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "crypto/equihash.h"
#include "primitives/block.h"
#include "streams.h"
#include "uint256.h"

#include <librustzcash.h>
#include <rust/equihash.h>

/**
 * Manually increase difficulty by a multiplier. Because of the use of compact
 * bits this is only an approximate increase, not 100% precise. (Ported from
 * Zclassic; used by the graduated fork-scaling rule below.)
 */
unsigned int IncreaseDifficultyBy(unsigned int nBits, int64_t multiplier, const Consensus::Params& params)
{
    arith_uint256 target;
    target.SetCompact(nBits);
    target /= multiplier;
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    if (target > pow_limit) {
        target = pow_limit;
    }
    return target.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Regtest
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    int nHeight = pindexLast->nHeight + 1;

    // Zclassic graduated fork-scaling: for the first nPowAveragingWindow blocks
    // after the DiffAdj and Buttercup upgrade forks, relax the difficulty based
    // on how far ahead of the previous block the candidate's timestamp is, so the
    // network can recover quickly from the spacing change.
    //
    // NOTE: the && / || grouping below is reproduced exactly from the Zclassic
    // reference (where '&&' binds tighter than '||'), so it parses as
    //   (scaleDifficultyAtUpgradeFork && <DiffAdj window>) || <Buttercup window>.
    // On mainnet scaleDifficultyAtUpgradeFork is true so both windows apply; this
    // grouping is preserved deliberately for bug-for-bug consensus parity.
    if ((params.scaleDifficultyAtUpgradeFork &&
         (nHeight >= params.vUpgrades[Consensus::UPGRADE_DIFFADJ].nActivationHeight &&
          nHeight < params.vUpgrades[Consensus::UPGRADE_DIFFADJ].nActivationHeight + params.nPowAveragingWindow)) ||
        (nHeight >= params.vUpgrades[Consensus::UPGRADE_BUTTERCUP].nActivationHeight &&
         nHeight < params.vUpgrades[Consensus::UPGRADE_BUTTERCUP].nActivationHeight + params.nPowAveragingWindow)) {

        if (pblock && pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.PoWTargetSpacing(nHeight) * 12) {
            // If > 12x spacing ahead, allow min difficulty.
            return nProofOfWorkLimit;
        } else if (pblock && pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.PoWTargetSpacing(nHeight) * 6) {
            // If > 6x spacing ahead, allow low estimate difficulty.
            return IncreaseDifficultyBy(nProofOfWorkLimit, 128, params);
        } else if (pblock && pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.PoWTargetSpacing(nHeight) * 2) {
            // If > 2x spacing ahead, allow high estimate difficulty.
            return IncreaseDifficultyBy(nProofOfWorkLimit, 256, params);
        }
        // Otherwise fall through to the normal averaging-window retarget.
    }

    {
        // Comparing to pindexLast->nHeight with >= because this function
        // returns the work required for the block after pindexLast.
        if (params.nPowAllowMinDifficultyBlocksAfterHeight != std::nullopt &&
            pindexLast->nHeight >= params.nPowAllowMinDifficultyBlocksAfterHeight.value())
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 6 * block interval minutes
            // then allow mining of a min-difficulty block.
            if (pblock && pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.PoWTargetSpacing(pindexLast->nHeight + 1) * 6)
                return nProofOfWorkLimit;
        }
    }

    // Find the first block in the averaging interval
    const CBlockIndex* pindexFirst = pindexLast;
    arith_uint256 bnTot {0};
    for (int i = 0; pindexFirst && i < params.nPowAveragingWindow; i++) {
        arith_uint256 bnTmp;
        bnTmp.SetCompact(pindexFirst->nBits);
        bnTot += bnTmp;
        pindexFirst = pindexFirst->pprev;
    }

    // Check we have enough blocks
    if (pindexFirst == NULL)
        return nProofOfWorkLimit;

    // The protocol specification leaves MeanTarget(height) as a rational, and takes the floor
    // only after dividing by AveragingWindowTimespan in the computation of Threshold(height):
    // <https://zips.z.cash/protocol/protocol.pdf#diffadjustment>
    //
    // Here we take the floor of MeanTarget(height) immediately, but that is equivalent to doing
    // so only after a further division, as proven in <https://math.stackexchange.com/a/147832/185422>.
    arith_uint256 bnAvg {bnTot / params.nPowAveragingWindow};

    return CalculateNextWorkRequired(bnAvg,
                                     pindexLast->GetMedianTimePast(), pindexFirst->GetMedianTimePast(),
                                     params,
                                     pindexLast->nHeight + 1);
}

unsigned int CalculateNextWorkRequired(arith_uint256 bnAvg,
                                       int64_t nLastBlockTime, int64_t nFirstBlockTime,
                                       const Consensus::Params& params,
                                       int nextHeight)
{
    int64_t averagingWindowTimespan = params.AveragingWindowTimespan(nextHeight);
    int64_t minActualTimespan = params.MinActualTimespan(nextHeight);
    int64_t maxActualTimespan = params.MaxActualTimespan(nextHeight);
    // Limit adjustment step
    // Use medians to prevent time-warp attacks
    int64_t nActualTimespan = nLastBlockTime - nFirstBlockTime;
    nActualTimespan = averagingWindowTimespan + (nActualTimespan - averagingWindowTimespan)/4;

    if (nActualTimespan < minActualTimespan) {
        nActualTimespan = minActualTimespan;
    }
    if (nActualTimespan > maxActualTimespan) {
        nActualTimespan = maxActualTimespan;
    }

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew {bnAvg};
    bnNew /= averagingWindowTimespan;
    bnNew *= nActualTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

bool CheckEquihashSolution(const CBlockHeader *pblock, const Consensus::Params& params)
{
    // Zclassic derives the Equihash (n, k) parameters from the solution size,
    // because the block header does not record which parameters were used and the
    // Zclassic chain contains both (200, 9) and (192, 7) blocks (the network moved
    // to 192,7 well before the formal Bubbles upgrade height). This matches the
    // reference's CheckEquihashSolution exactly. For any other solution size we
    // fall back to the configured parameters (e.g. custom regtest params), which
    // equihash::is_valid will then reject if they don't match.
    unsigned int n, k;
    size_t nSolSize = pblock->nSolution.size();
    if (nSolSize == 1344) {        // mainnet / testnet (Equihash 200,9)
        n = 200; k = 9;
    } else if (nSolSize == 400) {  // Equihash 192,7
        n = 192; k = 7;
    } else if (nSolSize == 68) {   // Equihash 96,5
        n = 96; k = 5;
    } else if (nSolSize == 36) {   // regtest genesis (Equihash 48,5)
        n = 48; k = 5;
    } else {
        n = params.nEquihashN;
        k = params.nEquihashK;
    }

    // I = the block header minus nonce and solution.
    CEquihashInput I{*pblock};
    // I||V
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;

    return equihash::is_valid(
        n, k,
        {(const unsigned char*)ss.data(), ss.size()},
        {pblock->nNonce.begin(), pblock->nNonce.size()},
        {pblock->nSolution.data(), pblock->nSolution.size()});
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;
    // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
    // as it's too large for an arith_uint256. However, as 2**256 is at least as large
    // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
    // or ~bnTarget / (bnTarget+1) + 1.
    return (~bnTarget / (bnTarget + 1)) + 1;
}

int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from, const CBlockIndex& tip, const Consensus::Params& params)
{
    arith_uint256 r;
    int sign = 1;
    if (to.nChainWork > from.nChainWork) {
        r = to.nChainWork - from.nChainWork;
    } else {
        r = from.nChainWork - to.nChainWork;
        sign = -1;
    }
    r = r * arith_uint256(params.PoWTargetSpacing(tip.nHeight)) / GetBlockProof(tip);
    if (r.bits() > 63) {
        return sign * std::numeric_limits<int64_t>::max();
    }
    return sign * r.GetLow64();
}
