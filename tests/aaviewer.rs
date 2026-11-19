//! The 192's side of the phone's video (`sdk/examples/aaviewer`) on an
//! emulated 192, with no firmware: the card is the one the SD bridge makes
//! up (`sdbridge/src/volume.c`, `aastream.c`), written to an image by the
//! bridge's host test program with `tests/data/margin.264` laid into
//! AALINK.BIN as a phone's messages would be, and the viewer, started
//! where its first stage (`sdk/examples/aaview`) would put it, finds the
//! file through its own card processor program, reads the blocks, decodes
//! every picture and shows it.  Each frame's CRC-32 (the viewer built with
//! -DAA_CRC) is checked against ffmpeg's (`tests/data/margin.crc`).
//!
//!     cargo test --release --test aaviewer -- --nocapture
//!
//! Needs an SH-4 GCC and a host C compiler; without either the test
//! returns at once and passes.  The bridge here is a card image, every
//! block already there: what the bridge does while the phone sends (the
//! ring, blocks not yet ready, the acknowledgements) is tested by
//! `sh sdbridge/tests/run.sh`.

use rnse::board::Model;
use rnse::machine::Machine;
use rnse::sh4::cpu::Stop;

fn build() -> Option<Vec<u8>> {
    let sdk = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("sdk");
    let out = std::process::Command::new("sh").arg(sdk.join("build.sh")).args(["192", "aaviewer"])
        .env("EXTRA_CFLAGS", "-DAA_CRC").output().ok()?;
    if !out.status.success() {
        let text = String::from_utf8_lossy(&out.stderr);
        assert!(!text.contains("error") || text.contains("not found"), "build: {}", text);
        return None;
    }
    let program = std::fs::read(sdk.join("out/aaviewer-192.bin")).unwrap();
    // The ordinary build back, for the card.
    let _ = std::process::Command::new("sh").arg(sdk.join("build.sh")).args(["192", "aaviewer"]).output();
    Some(program)
}

/// The bridge's card with the stream in it, from its host test program.
fn card(stream: &std::path::Path) -> Option<Vec<u8>> {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let dir = std::env::temp_dir().join(format!("rnse-aaviewer-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let tool = dir.join("host");
    let src = root.join("sdbridge/src");
    let cc = std::process::Command::new("cc").args(["-O2", "-o"]).arg(&tool).arg(root.join("sdbridge/tests/host.c"))
        .arg(src.join("card.c")).arg(src.join("volume.c")).arg(src.join("crc.c")).arg(src.join("aap.c"))
        .arg(src.join("aastream.c")).output().ok()?;
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));
    let image = dir.join("card.img");
    assert!(std::process::Command::new(&tool).arg(&image).arg(stream).status().unwrap().success());
    let card = std::fs::read(&image).unwrap();
    std::fs::remove_dir_all(&dir).ok();
    Some(card)
}

fn console(m: &Machine) -> String {
    String::from_utf8_lossy(&m.console).into_owned()
}

#[test]
fn viewer_decodes_the_bridge_s_stream() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build() else { return };
    let Some(card) = card(&root.join("tests/data/margin.264")) else { return };
    let want: Vec<String> = std::fs::read_to_string(root.join("tests/data/margin.crc")).unwrap()
        .lines().map(|l| l.trim().to_ascii_lowercase()).filter(|l| !l.is_empty()).collect();

    let mut m = Machine::for_model(Model::Rnse192);
    m.echo_console = std::env::var_os("RNSE_LINK_ECHO").is_some();
    m.scramble_ram();
    m.install_boot_stub("V222");
    // Where aaview puts it (sdk/examples/aaview/handoff.h), started at its
    // entry as image_enter would.
    m.board.load(0x0c01_0080, &program);
    m.cpu.reset();
    m.cpu.set_sr(0x7000_00f0);
    m.cpu.pc = u32::from_be_bytes([program[0], program[1], program[2], program[3]]);
    m.cpu.r[15] = 0x8c7a_fff0;
    m.busy_start();
    m.watchdog_always = true;
    m.cache_model();
    // The bridge in the second slot, as on the bench.
    assert!(m.insert_card(1, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));

    let ms = m.cycles_per_ms();
    let crcs = |m: &Machine| -> Vec<String> {
        console(m).lines().filter_map(|l| l.strip_prefix("aaviewer: frame "))
            .filter_map(|l| l.split(" crc ").nth(1)).map(|c| c.trim().to_ascii_lowercase()).collect()
    };
    for _ in 0..600 {
        if crcs(&m).len() >= want.len() {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let text = console(&m);
    println!("{}", text);
    // The screen, to look at: RNSE_AAVIEWER_SHOT=file.png.
    if let Some(path) = std::env::var_os("RNSE_AAVIEWER_SHOT") {
        let (w, h, rgb) = m.render();
        std::fs::write(path, rnse::png::encode_rgb(w, h, &rgb)).unwrap();
    }
    assert!(text.contains("BRIDGE IN SLOT 2"), "{}", text);
    let got = crcs(&m);
    assert_eq!(got.len(), want.len(), "{}", text);
    for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
        assert_eq!(g.trim_start_matches('0'), w.trim_start_matches('0'), "frame {}: {}", i, text);
    }
    // Nothing read again, nothing skipped: every block right the first time.
    m.run(3000 * ms);
    let text = console(&m);
    let last = text.lines().rev().find(|l| l.contains(" fps x10")).unwrap_or_else(|| panic!("no report: {}", text));
    assert!(last.contains("again 0 ") && last.contains("missing 0 ") && last.ends_with("skipped 0"), "{}", last);
}
