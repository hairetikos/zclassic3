use memuse::DynamicUsage;
use zcash_protocol::{
    consensus::{self, BlockHeight},
    local_consensus::LocalNetwork,
};

/// Chain parameters for the Zclassic networks supported by zclassicd.
///
/// Unlike upstream zcashd we must NOT use the hardcoded `consensus::Network::Main`/
/// `TestNetwork` parameters: those bake in Zcash's network-upgrade activation
/// heights, but Zclassic activates a different set of upgrades (in particular it
/// never activates Canopy or NU5). Getting these heights wrong on the Rust side
/// breaks height-dependent logic such as ZIP-212 enforcement during Sapling note
/// decryption -- with Zcash's heights, any Zclassic Sapling note mined above
/// Zcash's Canopy height would be (incorrectly) decrypted as a ZIP-212 note and
/// fail, making incoming shielded funds undetectable by the wallet.
///
/// We therefore always carry Zclassic's actual activation heights (held in a
/// `LocalNetwork`, which maps the `NetworkUpgrade` enum for us) and pair them with
/// the correct `NetworkType`, so address HRPs / key constants stay Zcash-compatible
/// on mainnet (as Zclassic intends) while the upgrade schedule is Zclassic's own.
#[derive(Clone, Copy)]
pub(crate) struct Network {
    network_type: consensus::NetworkType,
    upgrades: LocalNetwork,
}

impl DynamicUsage for Network {
    fn dynamic_usage(&self) -> usize {
        // `NetworkType` and `Option<BlockHeight>` allocate no heap memory.
        0
    }

    fn dynamic_usage_bounds(&self) -> (usize, Option<usize>) {
        (0, Some(0))
    }
}

/// Constructs a `Network` from the given network string and activation heights.
///
/// A negative height means the corresponding upgrade never activates, matching the
/// C++ `NO_ACTIVATION_HEIGHT` sentinel. The heights are always honoured (for all
/// network kinds), so the Rust consensus view matches zclassicd's C++ consensus.
#[allow(clippy::too_many_arguments)]
pub(crate) fn network(
    network: &str,
    overwinter: i32,
    sapling: i32,
    blossom: i32,
    heartwood: i32,
    canopy: i32,
    nu5: i32,
    nu6: i32,
    nu6_1: i32,
) -> Result<Box<Network>, &'static str> {
    let i32_to_optional_height = |n: i32| {
        if n.is_negative() {
            None
        } else {
            Some(BlockHeight::from_u32(n.unsigned_abs()))
        }
    };

    let network_type = match network {
        "main" => consensus::NetworkType::Main,
        "test" => consensus::NetworkType::Test,
        "regtest" => consensus::NetworkType::Regtest,
        _ => return Err("Unsupported network kind"),
    };

    let params = Network {
        network_type,
        upgrades: LocalNetwork {
            overwinter: i32_to_optional_height(overwinter),
            sapling: i32_to_optional_height(sapling),
            blossom: i32_to_optional_height(blossom),
            heartwood: i32_to_optional_height(heartwood),
            canopy: i32_to_optional_height(canopy),
            nu5: i32_to_optional_height(nu5),
            nu6: i32_to_optional_height(nu6),
            nu6_1: i32_to_optional_height(nu6_1),
        },
    };

    Ok(Box::new(params))
}

impl consensus::Parameters for Network {
    fn network_type(&self) -> consensus::NetworkType {
        self.network_type
    }

    fn activation_height(&self, nu: consensus::NetworkUpgrade) -> Option<consensus::BlockHeight> {
        // Delegate to the LocalNetwork, which holds Zclassic's activation heights
        // and maps the NetworkUpgrade enum internally.
        self.upgrades.activation_height(nu)
    }
}
