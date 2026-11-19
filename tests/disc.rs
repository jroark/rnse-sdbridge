//! The bridge's end-to-end tests: its 192 programs started the way a
//! unit starts them (the SD loader, put on an update disc in the loader's
//! place and started by the stock software) and run on the emulator.
//!
//!     RNSE_DISC=/path/to/disc.iso cargo test --release -p rnse-sdbridge -- --nocapture
//!
//! They need `RNSE_DISC`, an image of an update CD (firmware, never in a
//! repository), and an SH-4 GCC on the PATH (the kit's
//! `tools/sh4-docker` has one in a container); without either they
//! return early and pass, which proves nothing.  The kit is `RNSE_SDK`,
//! or rnse's `sdk/` beside this directory.  The helpers are copies of
//! rnse's `tests/disc.rs`, taking a program of this repository by path.

use rnse::fw;
use rnse::machine::Machine;
use rnse::sh4::cpu::Stop;

/// The kit: RNSE_SDK, or rnse's sdk/ beside this directory.
fn kit() -> std::path::PathBuf {
    std::env::var_os("RNSE_SDK").map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../rnse-sdk"))
}

/// A program of this repository, as build.sh takes it (a path).
fn own(program: &str) -> String {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(program).to_string_lossy().into_owned()
}

/// What build.sh names a program's output after: its last component.
fn output_name(example: &str) -> &str {
    example.rsplit('/').next().unwrap()
}

fn disc() -> Option<Vec<u8>> {
    let path = std::env::var_os("RNSE_DISC")?;
    Some(std::fs::read(path).expect("RNSE_DISC is not readable"))
}

/// A program written from scratch (sdk/examples/hello), built with an
/// SH-4 GCC if there is one, put in the loader's place on the disc and
/// started the way the boot ROM starts the loader: it brings the board
/// up, prints on the debug serial port, draws and keeps time, on both
/// units.
/// Build an SDK example for a unit, put it in the loader's place and
/// start it.  None when there is no cross compiler or the disc has no
/// software for that unit.
fn sdk_example(image: &[u8], model: rnse::board::Model, example: &str) -> Option<(Machine, String, Vec<u8>)> {
    use rnse::board::Model;
    let sdk = kit();
    let unit = if model == Model::Rnse192 { "192" } else { "193" };
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(model))?;
    // Builds of the same example for the two tests that use it must
    // not run into each other.
    static BUILD: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let built = {
        let _lock = BUILD.lock().unwrap();
        std::process::Command::new("sh").arg(sdk.join("build.sh")).args([unit, example]).output()
    };
    match built {
        Ok(out) if out.status.success() => {}
        Ok(out) => {
            // No cross compiler here: nothing to test.  A compiler that
            // is there and fails (a program that no longer fits, say)
            // is a failure, not a skip.
            let text = String::from_utf8_lossy(&out.stderr);
            assert!(text.contains("gcc: not found") || text.contains("gcc: command not found"),
                "{} for the {}: the build failed: {}", example, unit, text);
            return None;
        }
        Err(_) => return None,
    }
    let program = std::fs::read(sdk.join(format!("out/{}-{}.bin", output_name(example), unit))).unwrap();
    let (custom, places) = fw::custom_loader(image, &v.name, &program).unwrap();
    assert_eq!(places.len(), 2, "{}: the loader is on the disc in the container and as a file: {:?}", v.name, places);
    let variants = fw::load_variants(&custom).unwrap();
    let v = variants.iter().find(|x| x.name == v.name).unwrap();
    let ldm = v.module("LDM").unwrap();
    assert!(ldm.checksum_ok());

    let mut m = Machine::for_model(model);
    m.echo_console = false;
    // Whatever was in memory before: not zeroes.
    m.scramble_ram();
    m.install_boot_stub(&v.name);
    m.atapi().load(custom.clone());
    m.start_loader(ldm, [0; 3]);
    // As unforgiving as the emulator gets (`--strict`).
    m.busy_start();
    m.watchdog_always = true;
    m.cache_model();
    Some((m, v.name.clone(), custom))
}

/// An SDK example on a disc with a higher software index than the
/// installed one, and the installed (stock) software run until its
/// application has checked the disc, copied the program to RAM and is
/// about to call its first entry point.  That is how a real unit would
/// come to run it.  None when there is no cross compiler or the disc
/// has no software for the unit.
fn started_by_stock_software(image: &[u8], model: rnse::board::Model, example: &str) -> Option<(Machine, String, Vec<u8>)> {
    started_by_stock_software_with(image, model, example, None)
}

/// The same, with the disc in the drive from the start (None) or put
/// in after so many milliseconds, as someone would do it.
fn started_by_stock_software_with(image: &[u8], model: rnse::board::Model, example: &str, insert_at: Option<u64>) -> Option<(Machine, String, Vec<u8>)> {
    use rnse::board::Model;
    let sdk = kit();
    let variants = fw::load_variants(image).unwrap();
    let v = variants.iter().find(|v| Model::for_variant(&v.name) == Some(model))?;
    sdk_example(image, model, example)?;
    let unit = if model == Model::Rnse192 { "192" } else { "193" };
    let program = std::fs::read(sdk.join(format!("out/{}-{}.bin", output_name(example), unit))).unwrap();
    let (mut custom, _) = fw::custom_loader(image, &v.name, &program).unwrap();
    // The disc's index, its two middle digits one up: those are what
    // the application compares.
    let index = fw::IndexInfo::from_disc(image, &v.name).unwrap();
    let middle: u32 = index.sofidx[1..3].parse().unwrap();
    let higher = format!("{}{:02}{}", &index.sofidx[..1], middle + 1, &index.sofidx[3..]);
    assert_eq!(fw::set_disc_index(&mut custom, &v.name, &higher).unwrap(), index.sofidx);

    // The stock software's own start of a loader can fail: on the
    // European 192 the copy of the loader goes over a variable of the
    // module APFM, whose timer handler (every 100 ms) then meets an
    // address error if it runs in the 15 ms before the loader has
    // taken the unit over (docs/boot-rom.md).  Whether it does is a
    // matter of timing, which on a real unit the drive makes vary and
    // which here is the same in every run.  So the start is tried at
    // the nominal ratio of CPU speed to timers and, when exactly that
    // happens, at the two next to it.
    let apfm = v.module("APFM").map(|a| a.load_addr | 0x8000_0000..(a.load_addr | 0x8000_0000) + a.size);
    let nominal = if model == Model::Rnse192 { 4 } else { 8 };
    let mut started = None;
    for ratio in [nominal, nominal + 1, nominal - 1] {
        let mut m = Machine::for_model(model);
        m.set_cpu_ratio(ratio);
        m.echo_console = false;
        // The CPU's caches are modelled all the way: the stock software has
        // to live with them as on a real unit, and so has the program.
        m.scramble_ram();
        m.cache_model();
        m.install_boot_stub(&v.name);
        m.install(v, &index);
        if insert_at.is_none() {
            m.atapi().load(custom.clone());
        }
        m.start_os(v).unwrap();
        m.set_ignition(true);
        let ms = m.cycles_per_ms();
        let entry = fw::load_variants(&custom).unwrap().iter().find(|x| x.name == v.name).unwrap().module("LDM").unwrap().entries[0];
        m.cpu.breakpoints.push(entry);
        let mut stop = Stop::Budget;
        if let Some(at) = insert_at {
            stop = m.run(at * ms);
            assert_eq!(stop, Stop::Budget, "{}: {:?}", v.name, m.cpu.fault);
            m.atapi().insert(Some(custom.clone()));
        }
        for _ in 0..40 {
            if stop != Stop::Budget {
                break;
            }
            stop = m.run(1_000 * ms);
        }
        let collision = stop == Stop::Fault && m.cpu.fault.as_deref().is_some_and(|f| {
            let spc = f.split("SPC=").nth(1).and_then(|t| u32::from_str_radix(&t[..8.min(t.len())], 16).ok());
            f.contains("EXPEVT=0e0") && spc.zip(apfm.clone()).is_some_and(|(spc, range)| range.contains(&spc))
        });
        if collision {
            eprintln!("{}: at CPU ratio {} the stock software's start of the loader collides with its own timer handler", v.name, ratio);
            continue;
        }
        assert_eq!(stop, Stop::Breakpoint, "{}: the installed software did not start the program: {:?}", v.name, m.cpu.fault);
        started = Some(m);
        break;
    }
    let mut m = started.unwrap_or_else(|| panic!("{}: the stock software's start of the loader failed at every speed tried", v.name));
    m.cpu.breakpoints.clear();
    Some((m, v.name.clone(), custom))
}

/// Run until the log has `what` after `from`, in steps of five seconds.
fn until(m: &mut Machine, from: usize, what: &str, seconds: u64) -> Option<usize> {
    let ms = m.cycles_per_ms();
    for _ in 0..seconds / 5 {
        assert_eq!(m.run(5_000 * ms), Stop::Budget, "{:?}", m.cpu.fault);
        let text = String::from_utf8_lossy(&m.console);
        if let Some(at) = text.get(from..).and_then(|t| t.find(what)) {
            return Some(from + at);
        }
    }
    None
}

/// Android Auto's video on the 192 (sdk/examples/h264bench): the
/// stock software starts the SD loader, the loader the benchmark, which
/// decodes an H.264 stream from the card as a phone would send it
/// (tests/data/margin.264: twelve synthetic frames of 800x480 made by
/// tools/aa-streams.sh) and finds every frame as ffmpeg decodes it
/// (margin.crc, the CRC-32s ffmpeg's frames have), twice: with the
/// caches as the stock software leaves them and the decoder as it came,
/// and with the caches set up anew (`cache_setup`: index modes off, P1
/// copy-back) and the decoder's direct copy, under the modelled caches.
#[test]
fn sdk_decodes_h264_from_a_card() {
    use rnse::board::Model;
    let Some(image) = disc() else { return };
    let sdk = kit();
    let Some((mut m, name, _)) = started_by_stock_software(&image, Model::Rnse192, "sdboot") else { return };
    if sdk_example(&image, Model::Rnse192, &own("h264bench")).is_none() { return }
    let bench = std::fs::read(sdk.join("out/h264bench-192.bin")).unwrap();
    let entries = vec![
        rnse::fat::Entry::file("RNSE.BIN", bench),
        rnse::fat::Entry::file("MARGIN.264", include_bytes!("data/margin.264").to_vec()),
        rnse::fat::Entry::file("MARGIN.CRC", include_bytes!("data/margin.crc").to_vec()),
    ];
    let card = rnse::fat::card("VIDEO", &entries, None).unwrap();
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let flash = m.board.flash.clone();
    let ms = m.cycles_per_ms();
    m.cpu.breakpoints.clear();
    let text = loop {
        assert_eq!(m.run(1_000 * ms), Stop::Budget, "{}: {:?}", name, m.cpu.fault);
        let text = String::from_utf8_lossy(&m.console).into_owned();
        if text.contains("DONE.") || m.cpu.cycles > 120_000 * ms {
            break text;
        }
    };
    let tail = &text[text.len().saturating_sub(1200)..];
    assert!(text.contains("rnse sdk: h264bench"), "{}: {:?}", name, tail);
    // Each frame's line is the CRC-32 of the frames up to it, which is
    // what each line of the .CRC file has.
    let want: Vec<&str> = std::str::from_utf8(include_bytes!("data/margin.crc")).unwrap().lines().collect();
    assert_eq!(want.len(), 12);
    for crc in &want {
        assert_eq!(text.matches(&format!("{} ", crc)).count(), 2, "{}: frame CRC {} not seen once a pass: {:?}", name, crc, tail);
    }
    let rows: Vec<&str> = text.lines().filter(|l| l.contains("MARGIN ") && (l.starts_with("STOCK ") || l.starts_with("COPY+CB+NOIX "))).collect();
    assert_eq!(rows.len(), 2, "{}: {:?}", name, tail);
    for row in rows {
        assert!(row.ends_with(" OK"), "{}: {}", name, row);
    }
    assert!(!text.contains("unexpected"), "{}: {:?}", name, tail);
    // And back to the SD loader, with the caches as the program found
    // them and the flash as it was.
    let at = text.find("DONE.").unwrap();
    until(&mut m, at, "TEST RETURNED: 0", 90).unwrap_or_else(|| panic!("{}: not back at the loader: {:?}", name, String::from_utf8_lossy(&m.console[at..])));
    assert_eq!(m.board.flash, flash, "{}", name);
    let cache = &m.cpu.cache.trouble;
    assert_eq!((cache.stale_fetches, cache.dirty_discarded), (0, 0), "{}: {:?}", name, cache);
}

/// The slot benchmark (`sdk/examples/linkbench`) as it was run on a unit
/// on 2026-11-16: the SD loader started by the stock software, a card
/// with LINKTEST.BIN in the first slot (there the RP2040 bridge) and the
/// benchmark as LINKBENC.BIN in the second.  The stock way runs first,
/// then the benchmark's own card processor program takes over from the
/// stock one with its pins (which tests/linkbench.rs, with no stock
/// software, never does).
#[test]
fn link_benchmark_from_the_sd_loader() {
    use rnse::board::Model;
    let Some(image) = disc() else { return };
    let sdk = kit();
    let Some((mut m, name, _)) = started_by_stock_software(&image, Model::Rnse192, "sdboot") else { return };
    if sdk_example(&image, Model::Rnse192, &own("linkbench")).is_none() { return }
    let bench = std::fs::read(sdk.join("out/linkbench-192.bin")).unwrap();
    let ms = m.cycles_per_ms();
    m.cpu.breakpoints.clear();
    assert_eq!(m.run(2000 * ms), Stop::Budget);
    // LINKTEST.BIN as tools/linktest.py writes it.
    let file: Vec<u8> = (0..(2u32 << 20) / 4).flat_map(|j| {
        let mut x = j.wrapping_add(1).wrapping_mul(0x9e37_79b1);
        x ^= x >> 16;
        x = x.wrapping_mul(0x85eb_ca6b);
        x ^= x >> 13;
        x.to_be_bytes()
    }).collect();
    let link = rnse::fat::card("LINK", &[rnse::fat::Entry::file("LINKTEST.BIN", file)], None).unwrap();
    // An old card, as the 32 MB one tried on a unit: no answer to CMD8,
    // busy for a few rounds of ACMD41.
    m.board.media.old_cards = true;
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(link).unwrap()));
    m.board.media.old_cards = false;
    assert_eq!(m.run(2000 * ms), Stop::Budget);
    let card = rnse::fat::card("RNSE", &[rnse::fat::Entry::file("LINKBENC.BIN", bench.clone())], None).unwrap();
    let at = m.console.len();
    // The only program on the card: the loader starts it by itself.
    assert!(m.insert_card(1, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let started = until(&mut m, at, "rnse sdk: linkbench", 30).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[at..])));
    until(&mut m, started, "DONE.", 400).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[started..])));
    let text = String::from_utf8_lossy(&m.console[started..]).into_owned();
    println!("{}", text);
    assert!(text.contains("SLOT 1, STOCK PROGRAM"), "{}: {}", name, text);
    assert!(text.contains("CARD STARTED") && !text.contains("OWN PROGRAM: STEP"), "{}: {}", name, text);
    let summary = &text[text.find("LINKBENCH 1: KB/S").unwrap()..];
    // (Not the comparison for a unit with commands started the old way,
    // "OLD", which on a unit and here gets no response interrupt.)
    for line in summary.lines().filter(|l| l.contains(" KB/S ") && !l.contains(" OLD ")) {
        assert!(line.contains(" B0 T0 L0") && !line.contains("FAIL"), "{}: {}", name, line);
    }
    assert!(!summary.contains("NOT RUN"), "{}: {}", name, summary);
    // The card in the other slot has the program: the results go through
    // it (as through the RP2040 bridge, whose log has them), and the
    // program waits for a new one there and runs it (a bridge gets it
    // over USB from sdbridge/link.py; here the card is changed).
    assert!(text.contains("SLOT 2: LINKBENC.BIN ") && text.contains("RESULTS THROUGH THE OTHER CARD"), "{}: {}", name, text);
    until(&mut m, started, "WAITING FOR A NEW LINKBENC.BIN IN SLOT 2", 60).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[started..])));
    assert_eq!(m.run(5000 * ms), Stop::Budget);
    assert!(!String::from_utf8_lossy(&m.console[started..]).contains("A NEW ONE"));
    let mut newer = bench;
    newer.extend_from_slice(&[0; 4]);
    let card = rnse::fat::card("RNSE", &[rnse::fat::Entry::file("LINKBENC.BIN", newer)], None).unwrap();
    m.eject_card(1);
    assert!(m.insert_card(1, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let again = until(&mut m, started, "A NEW ONE: STARTING IT", 60).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[started..])));
    let again = until(&mut m, again, "rnse sdk: linkbench", 30).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[again..])));
    until(&mut m, again, "RESULTS THERE", 60).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[again..])));
}

/// The phone's video on the 192 as a unit runs it (sdbridge/,
/// SDLINK.UF2; sdk/examples/aaview and aaviewer): the SD loader started by
/// the stock software, the bridge in the second slot (its card made up by
/// the bridge's own code, sdbridge/tests/host.c, with tests/data/margin.264
/// laid into AALINK.BIN as a phone's messages), a card in the first with
/// AAVIEW.BIN and AAVIEWER.IMG.  The loader starts AAVIEW.BIN, that loads
/// the viewer and enters it, the viewer finds the bridge through the
/// stock card program, takes the slot over with its own, decodes every
/// picture, each frame's CRC-32 as ffmpeg's (the viewer built with
/// -DAA_CRC), and plays the stream's sound (a tone) on the other card
/// processor.  RETURN held goes back to the loader.  Needs a host C
/// compiler as well (tests/aaviewer.rs has the viewer with no firmware).
#[test]
fn android_auto_viewer_from_the_sd_loader() {
    use rnse::board::Model;
    let Some(image) = disc() else { return };
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let sdk = kit();
    let Some((mut m, name, _)) = started_by_stock_software(&image, Model::Rnse192, "sdboot") else { return };
    if sdk_example(&image, Model::Rnse192, &own("aaview")).is_none() { return }
    let first = std::fs::read(sdk.join("out/aaview-192.bin")).unwrap();
    let built = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("192").arg(own("aaviewer"))
        .env("EXTRA_CFLAGS", "-DAA_CRC").output().unwrap();
    assert!(built.status.success(), "{}", String::from_utf8_lossy(&built.stderr));
    let viewer = std::fs::read(sdk.join("out/aaviewer-192.bin")).unwrap();
    let _ = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("192").arg(own("aaviewer")).output();
    // The bridge's card, from its host test program.
    let dir = std::env::temp_dir().join(format!("rnse-aaview-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let tool = dir.join("host");
    let src = root.join("src");
    let Ok(cc) = std::process::Command::new("cc").args(["-O2", "-o"]).arg(&tool).arg(root.join("tests/host.c"))
        .arg(src.join("card.c")).arg(src.join("volume.c")).arg(src.join("crc.c")).arg(src.join("aap.c"))
        .arg(src.join("aastream.c")).output() else { return };
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));
    let bridge_image = dir.join("bridge.img");
    // With 2 s of a 1 kHz tone as the phone's media sound.
    assert!(std::process::Command::new(&tool).arg(&bridge_image).arg(root.join("tests/data/margin.264")).arg("2000").status().unwrap().success());
    let bridge = std::fs::read(&bridge_image).unwrap();
    std::fs::remove_dir_all(&dir).ok();
    let want: Vec<String> = std::fs::read_to_string(root.join("tests/data/margin.crc")).unwrap()
        .lines().map(|l| l.trim().to_ascii_lowercase()).filter(|l| !l.is_empty()).collect();

    let ms = m.cycles_per_ms();
    m.cpu.breakpoints.clear();
    assert_eq!(m.run(2000 * ms), Stop::Budget);
    m.speaker = Some(Vec::new());
    assert!(m.insert_card(1, rnse::dev::sdhi::Card::from_bytes(bridge).unwrap()));
    assert_eq!(m.run(2000 * ms), Stop::Budget);
    let card = rnse::fat::card("RNSE", &[rnse::fat::Entry::file("AAVIEW.BIN", first), rnse::fat::Entry::file("AAVIEWER.IMG", viewer)], None).unwrap();
    let at = m.console.len();
    // The only program on the card: the loader starts it by itself.
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    let started = until(&mut m, at, "rnse sdk: aaviewer", 60).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[at..])));
    let crcs = |m: &Machine| -> Vec<String> {
        String::from_utf8_lossy(&m.console[started..]).lines().filter_map(|l| l.strip_prefix("aaviewer: frame "))
            .filter_map(|l| l.split(" crc ").nth(1)).map(|c| c.trim().to_ascii_lowercase()).collect()
    };
    for _ in 0..1200 {
        if crcs(&m).len() >= want.len() {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{}: {:?}: {}", name, m.cpu.fault, String::from_utf8_lossy(&m.console[started..]));
    }
    // The sound: the tone heard through the amplifier's decoder input
    // (at the viewer's starting volume: quiet, but its pitch is there).
    assert_eq!(m.run(2500 * ms), Stop::Budget);
    let heard = m.speaker.take().unwrap();
    let right: Vec<i16> = heard.iter().skip(1).step_by(2).copied().collect();
    let pitches: Vec<u32> = right.chunks(22_050).map(|c| 2 * c.windows(2).filter(|w| w[0] < 0 && w[1] >= 0).count() as u32).collect();
    assert!(pitches.iter().filter(|p| p.abs_diff(1000) <= 8).count() >= 2, "{}: the tone: {:?}", name, pitches);
    let text = String::from_utf8_lossy(&m.console[started..]).into_owned();
    println!("{}", text);
    assert!(text.contains("BRIDGE IN SLOT 2"), "{}: {}", name, text);
    let got = crcs(&m);
    assert_eq!(got.len(), want.len(), "{}: {}", name, text);
    for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
        assert_eq!(g.trim_start_matches('0'), w.trim_start_matches('0'), "{}: frame {}: {}", name, i, text);
    }
    // RETURN held: back to the loader, through aaview.
    let before = m.console.len();
    assert!(m.panel_key("RETURN", true));
    assert_eq!(m.run(2500 * ms), Stop::Budget);
    assert!(m.panel_key("RETURN", false));
    until(&mut m, before, "TEST ENDED BY HOLDING RETURN", 30).unwrap_or_else(|| panic!("{}: {}", name, String::from_utf8_lossy(&m.console[before..])));
}
