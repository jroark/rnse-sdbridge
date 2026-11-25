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
    // Once for all the tests here, which run side by side.
    static BUILT: std::sync::Mutex<Option<Option<Vec<u8>>>> = std::sync::Mutex::new(None);
    let mut built = BUILT.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(program) = built.as_ref() {
        return program.clone();
    }
    let program = build_once();
    *built = Some(program.clone());
    program
}

/// The viewer built for the 193 (-DAA_CRC), once for the tests here.
fn build_193() -> Option<Vec<u8>> {
    static BUILT: std::sync::Mutex<Option<Option<Vec<u8>>>> = std::sync::Mutex::new(None);
    let mut built = BUILT.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(program) = built.as_ref() {
        return program.clone();
    }
    let here = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let sdk = std::env::var_os("RNSE_SDK").map(std::path::PathBuf::from).unwrap_or_else(|| here.join("../rnse-sdk"));
    let program = (|| {
        let out = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("193").arg(here.join("aaviewer"))
            .env("EXTRA_CFLAGS", "-DAA_CRC").output().ok()?;
        if !out.status.success() {
            let text = String::from_utf8_lossy(&out.stderr);
            assert!(text.contains("gcc: not found") || text.contains("gcc: command not found"), "build: {}", text);
            return None;
        }
        let program = std::fs::read(sdk.join("out/aaviewer-193.bin")).unwrap();
        let _ = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("193").arg(here.join("aaviewer")).output();
        Some(program)
    })();
    *built = Some(program.clone());
    program
}

fn build_once() -> Option<Vec<u8>> {
    let here = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    // The kit: RNSE_SDK, or ../rnse-sdk.
    let sdk = std::env::var_os("RNSE_SDK").map(std::path::PathBuf::from).unwrap_or_else(|| here.join("../rnse-sdk"));
    let out = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("192").arg(here.join("aaviewer"))
        .env("EXTRA_CFLAGS", "-DAA_CRC").output().ok()?;
    if !out.status.success() {
        let text = String::from_utf8_lossy(&out.stderr);
        assert!(text.contains("gcc: not found") || text.contains("gcc: command not found"), "build: {}", text);
        return None;
    }
    let program = std::fs::read(sdk.join("out/aaviewer-192.bin")).unwrap();
    // The ordinary build back, for the card.
    let _ = std::process::Command::new("sh").arg(sdk.join("build.sh")).arg("192").arg(here.join("aaviewer")).output();
    Some(program)
}

/// The bridge's card with the stream in it, from its host test program.
fn card(stream: &std::path::Path) -> Option<Vec<u8>> {
    card_with_sound(stream, 0)
}

/// The same with `ms` milliseconds of a 1 kHz tone as the phone's media
/// sound, between the pictures and after them.
fn card_with_sound(stream: &std::path::Path, ms: u32) -> Option<Vec<u8>> {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("rnse-aaviewer-{}-{}", std::process::id(), n));
    std::fs::create_dir_all(&dir).unwrap();
    let tool = dir.join("host");
    let src = root.join("src");
    let cc = std::process::Command::new("cc").args(["-O2", "-o"]).arg(&tool).arg(root.join("tests/host.c"))
        .arg(src.join("card.c")).arg(src.join("volume.c")).arg(src.join("crc.c")).arg(src.join("aap.c"))
        .arg(src.join("aastream.c")).output().ok()?;
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));
    let image = dir.join("card.img");
    let mut run = std::process::Command::new(&tool);
    run.arg(&image).arg(stream);
    if ms > 0 {
        run.arg(ms.to_string());
    }
    assert!(run.status().unwrap().success());
    let card = std::fs::read(&image).unwrap();
    std::fs::remove_dir_all(&dir).ok();
    Some(card)
}

/// The viewer started where the phone is under way: the codec's
/// configuration and the key frame gone from the bridge (`tail` past
/// them), as when the board has run on since a viewer before.
fn mid_stream(card: &mut [u8], tail: u32) {
    // AALINK.BIN's first sector: the bridge's volume puts it at 16801
    // (sdbridge/src/volume.c); its state's key and tail.
    let at = 16801 * 512;
    assert_eq!(&card[at..at + 4], b"AALC");
    card[at + 12..at + 16].copy_from_slice(&tail.to_be_bytes());
    card[at + 16..at + 20].copy_from_slice(&0xffff_ffffu32.to_be_bytes());
}

fn console(m: &Machine) -> String {
    String::from_utf8_lossy(&m.console).into_owned()
}

fn start(program: &[u8], card: Vec<u8>) -> Machine {
    let mut m = Machine::for_model(Model::Rnse192);
    m.echo_console = std::env::var_os("RNSE_LINK_ECHO").is_some();
    m.scramble_ram();
    m.install_boot_stub("V222");
    m.board.load(0x0c01_0080, program);
    m.cpu.reset();
    m.cpu.set_sr(0x7000_00f0);
    m.cpu.pc = u32::from_be_bytes([program[0], program[1], program[2], program[3]]);
    m.cpu.r[15] = 0x8c7a_fff0;
    m.busy_start();
    m.watchdog_always = true;
    m.cache_model();
    assert!(m.insert_card(1, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    m
}

/// Started in the middle of the phone's stream with its last key frame
/// gone: the viewer asks for one, skips the pictures that cannot be
/// decoded without it, and keeps going (here no key frame comes: the
/// card is an image).
#[test]
fn viewer_started_mid_stream_waits_for_a_key_frame() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build() else { return };
    let Some(mut card) = card(&root.join("tests/data/margin.264")) else { return };
    mid_stream(&mut card, 4);
    let mut m = start(&program, card);
    let ms = m.cycles_per_ms();
    for _ in 0..60 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let text = console(&m);
    println!("{}", text);
    assert!(!text.contains("EXCEPTION") && !text.contains("aaviewer: frame "), "{}", text);
    let last = text.lines().rev().find(|l| l.contains(" fps x10")).unwrap_or_else(|| panic!("no report: {}", text));
    assert!(last.starts_with("aaviewer: 0 frames") && !last.contains("skipped 0;"), "{}", last);
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
    // The stream's colours said once (no colour description: BT.601,
    // studio range, matrix 2 "unspecified").
    assert!(text.contains("aaviewer: colour: studio range, BT.601 (matrix 2)"), "{}", text);
    let got = crcs(&m);
    assert_eq!(got.len(), want.len(), "{}", text);
    for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
        assert_eq!(g.trim_start_matches('0'), w.trim_start_matches('0'), "frame {}: {}", i, text);
    }
    // Nothing read again, nothing skipped: every block right the first time.
    m.run(3000 * ms);
    let text = console(&m);
    let last = text.lines().rev().find(|l| l.contains(" fps x10")).unwrap_or_else(|| panic!("no report: {}", text));
    assert!(last.contains("again 0 ") && last.contains("missing 0 ") && last.contains("skipped 0;"), "{}", last);
}

/// The unit's knob and keys for the phone: each an input sector read
/// (aalink.h, "Input"), the select knob as the rotary controller, its
/// push as ENTER, a short RETURN as BACK.
#[test]
fn viewer_sends_the_knob_and_keys() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build() else { return };
    let Some(card) = card(&root.join("tests/data/margin.264")) else { return };
    let mut m = start(&program, card);
    let ms = m.cycles_per_ms();
    for _ in 0..80 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let at = m.console.len();
    assert!(m.panel_knob("main", 2));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    assert!(m.panel_key("SELECT", true));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    assert!(m.panel_key("SELECT", false));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    assert!(m.panel_key("RETURN", true));
    assert_eq!(m.run(200 * ms), Stop::Budget);
    assert!(m.panel_key("RETURN", false));
    assert_eq!(m.run(500 * ms), Stop::Budget);
    let text = String::from_utf8_lossy(&m.console[at..]).into_owned();
    let what: Vec<u32> = text.lines().filter_map(|l| l.strip_prefix("aaviewer: input "))
        .map(|c| u32::from_str_radix(c.trim(), 16).unwrap() & 0x3ff).collect();
    // The knob's two detents (in one event or two), ENTER down and up,
    // BACK down and up.
    // (Kind in bits 9-8: 0 a key, 1 the knob, 2 the backlog, said too.)
    let knob: i32 = what.iter().filter(|w| *w & 0x300 == 0x100).map(|w| (*w & 0xff) as u8 as i8 as i32).sum();
    let keys: Vec<u32> = what.iter().copied().filter(|w| w & 0x300 == 0).collect();
    assert_eq!(knob, 2, "{}", text);
    assert_eq!(keys, vec![6 << 1 | 1, 6 << 1, 1 << 1 | 1, 1 << 1], "{}", text);
}

/// The steering wheel (the kit's car driver, CAN 0x5C3 `39 KK`): seek
/// up and down go to the phone as NEXT and PREV, the volume keys turn
/// the unit's volume as the knob does.
#[test]
fn viewer_follows_the_steering_wheel() {
    use rnse::dev::hcan::Frame;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build() else { return };
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 8000) else { return };
    let mut m = start(&program, card);
    let ms = m.cycles_per_ms();
    for _ in 0..80 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let at = m.console.len();
    let volume = m.radio.volume;
    for code in [0x04u8, 0x00, 0x05, 0x00, 0x06, 0x00, 0x06, 0x00] {
        m.can_send_cyclic(Frame::new(0x5c3, &[0x39, code]), 100);
        assert_eq!(m.run(300 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert_eq!(m.run(500 * ms), Stop::Budget);
    let text = String::from_utf8_lossy(&m.console[at..]).into_owned();
    let keys: Vec<u32> = text.lines().filter_map(|l| l.strip_prefix("aaviewer: input "))
        .map(|c| u32::from_str_radix(c.trim(), 16).unwrap() & 0x3ff).filter(|w| w & 0x300 == 0).collect();
    // NEXT (8) down and up, PREV (9) down and up.
    assert_eq!(keys, vec![8 << 1 | 1, 8 << 1, 9 << 1 | 1, 9 << 1], "{}", text);
    // Two short presses of volume up: two steps of the amplifier.
    assert_eq!(m.radio.volume, volume + 2, "{}", text);
}

/// The steering wheel on the 193: the kit's car driver takes the CAN
/// controller as the stock software left it (can_attach), and seek up
/// and down go to the phone as NEXT and PREV, the volume keys turn the
/// unit's volume.
#[test]
fn viewer_follows_the_steering_wheel_on_the_193() {
    use rnse::dev::hcan::Frame;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 8000) else { return };
    let mut m = start_193(&program, card);
    // As the stock software leaves it: running, at its bit timing.
    m.can().start();
    let ms = m.cycles_per_ms();
    for _ in 0..200 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let at = m.console.len();
    let volume = m.radio.volume;
    for code in [0x04u8, 0x00, 0x05, 0x00, 0x06, 0x00, 0x06, 0x00] {
        m.can_send_cyclic(Frame::new(0x5c3, &[0x39, code]), 100);
        assert_eq!(m.run(300 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert_eq!(m.run(500 * ms), Stop::Budget);
    let text = String::from_utf8_lossy(&m.console[at..]).into_owned();
    let keys: Vec<u32> = text.lines().filter_map(|l| l.strip_prefix("aaviewer: input "))
        .map(|c| u32::from_str_radix(c.trim(), 16).unwrap() & 0x3ff).filter(|w| w & 0x300 == 0).collect();
    assert_eq!(keys, vec![8 << 1 | 1, 8 << 1, 9 << 1 | 1, 9 << 1], "{}", text);
    assert_eq!(m.radio.volume, volume + 2, "{}", text);
    // And the report counts the wheel's changes (on the bench, CAN 0xd7).
    let wheel: Vec<u32> = console(&m).lines().filter_map(|l| l.split("; wheel ").nth(1)).filter_map(|n| n.trim().parse().ok()).collect();
    assert!(wheel.last().is_some_and(|&n| n > 0), "{:?}", wheel);
}

/// A viewer started from its own disc in a car must not keep the unit
/// awake with the key out: once the ignition has been seen on, ten
/// seconds of the key out end it with a record (0xda) and a restart, into
/// the stock software.  A key that comes back within that time changes
/// nothing, and neither does a bus that never showed an ignition (a unit
/// alone on a bench).
#[test]
fn viewer_hands_back_to_stock_at_key_off_on_the_193() {
    use rnse::dev::hcan::Frame;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 3000) else { return };
    let gone = |m: &mut Machine| m.can().tx.iter().any(|f| f.id == 0x7f8 && f.data[0] == 0xda);

    // No ignition message at all: it runs on.
    let mut m = start_193(&program, card.clone());
    m.can().start();
    let ms = m.cycles_per_ms();
    for _ in 0..250 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert!(!gone(&mut m), "{}", console(&m));
    assert_eq!(m.resets, 0);

    let mut m = start_193(&program, card);
    m.can().start();
    m.can_send_cyclic(Frame::new(0x271, &[0x03]), 100);
    for _ in 0..200 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    // Out for five seconds and in again: nothing.
    m.can_send_cyclic(Frame::new(0x271, &[0x00]), 100);
    for _ in 0..50 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    m.can_send_cyclic(Frame::new(0x271, &[0x03]), 100);
    for _ in 0..120 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert!(!gone(&mut m), "{}", console(&m));
    // Out for good: the record within ten to eleven seconds, then the restart.
    m.can_send_cyclic(Frame::new(0x271, &[0x00]), 100);
    let mut after = None;
    let mut restarted = false;
    for k in 0..130 {
        restarted |= m.run(100 * ms) != Stop::Budget;
        if gone(&mut m) {
            after = Some(k);
            break;
        }
    }
    let after = after.unwrap_or_else(|| panic!("no hand-back: {}", console(&m)));
    assert!((95..=115).contains(&after), "after {} tenths of a second", after);
    let d = m.can().tx.iter().find(|f| f.id == 0x7f8 && f.data[0] == 0xda).unwrap().data;
    assert_eq!(&d[..8], &[0xda, 0, 0, 0, 0, 10, 0, 0]);
    // (The emulator stops at the jump to the reset entry: the boot
    // ROM's, which is not part of it.)
    for _ in 0..20 {
        if restarted || m.resets > 0 {
            break;
        }
        restarted |= m.run(100 * ms) != Stop::Budget;
    }
    assert!(restarted || m.resets > 0, "still in the viewer: pc {:08x}", m.cpu.pc);
}

/// The phone's sound on the 193: the same 1 kHz tone (the left at half
/// the right's level) through the chip's own sound interface (the kit's
/// pcm_stream there), the amplifier on the decoder's input at the volume
/// the knob sets, without a break.
#[test]
fn viewer_plays_the_phone_s_sound_on_the_193() {
    use rnse::dev::radio::Radio;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 3000) else { return };
    let mut m = start_193(&program, card);
    m.speaker = Some(Vec::new());
    let ms = m.cycles_per_ms();
    for _ in 0..60 {
        if console(&m).contains("BRIDGE IN SLOT") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert!(m.panel_knob("volume", 18));
    for _ in 0..60 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert_eq!(m.radio.volume, 30, "the volume knob");
    let text = console(&m);
    assert!(text.contains("aaviewer: frame 11 "), "{}", text);
    assert_eq!(m.radio.source, Radio::SOURCE_DECODER, "{}", text);
    let heard = m.speaker.take().unwrap();
    let right: Vec<i16> = heard.iter().skip(1).step_by(2).copied().collect();
    let left: Vec<i16> = heard.iter().step_by(2).copied().collect();
    let from = right.iter().position(|v| v.abs() > 8000).unwrap_or_else(|| panic!("no tone heard ({} samples): {}", right.len(), text));
    let tone = &right[from..(from + 110_250).min(right.len())];
    assert!(tone.len() >= 88_200, "only {} samples of the tone: {}", tone.len(), text);
    let pitches: Vec<u32> = tone.chunks(22_050).map(|c| 2 * c.windows(2).filter(|w| w[0] < 0 && w[1] >= 0).count() as u32).collect();
    for p in &pitches[..pitches.len() - 1] {
        assert!(p.abs_diff(1000) <= 8, "1 kHz each half second: {:?}", pitches);
    }
    let peak = tone.iter().map(|v| i32::from(*v).abs()).max().unwrap();
    let largest = tone.windows(2).map(|w| (i32::from(w[1]) - i32::from(w[0])).abs()).max().unwrap();
    let slope = (1.2 * 2.0 * std::f64::consts::PI * 1000.0 / 44_100.0 * f64::from(peak)) as i32;
    assert!(peak > 15_000 && largest <= slope + 2, "a step of {} where the sine of {} has at most {}", largest, peak, slope);
    let lpeak = left[from..from + tone.len()].iter().map(|v| i32::from(*v).abs()).max().unwrap();
    assert!((lpeak * 2 - peak).abs() < peak / 20, "left {} right {}", lpeak, peak);
    // The report: no gap in the sound (on the bench, CAN 0xd6).
    let gaps: Vec<u32> = text.lines().filter_map(|l| l.split(" gaps ").nth(1)).filter_map(|n| n.split(';').next()?.trim().parse().ok()).collect();
    assert!(!gaps.is_empty() && gaps.iter().all(|&g| g == 0), "gaps: {:?}", gaps);
}

/// A new program over CAN while the viewer runs on the 193 (the kit's
/// canload, as `canbench run ... --push FILE` sends it): the image in
/// numbered frames of seven bytes, acknowledged every 32; the viewer
/// checks the CRC, stops its sound and starts it.  Here the kit's
/// examples/hello, which says so on the console.
#[test]
fn viewer_takes_a_new_program_over_can_on_the_193() {
    use rnse::dev::hcan::Frame;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let sdk = std::env::var_os("RNSE_SDK").map(std::path::PathBuf::from).unwrap_or_else(|| root.join("../rnse-sdk"));
    let out = std::process::Command::new("sh").arg(sdk.join("build.sh")).args(["193", "hello"]).current_dir(&sdk).output().unwrap();
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    let image = std::fs::read(sdk.join("out/hello-193.bin")).unwrap();
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 3000) else { return };
    let mut m = start_193(&program, card);
    m.can().start();
    let ms = m.cycles_per_ms();
    for _ in 0..200 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let answers = |m: &mut Machine| -> Vec<[u8; 8]> {
        let a: Vec<[u8; 8]> = m.can().tx.iter().filter(|f| f.id == 0x7f8 && f.len == 8 && (0xc0..=0xc2).contains(&f.data[0]))
            .map(|f| { let mut d = [0u8; 8]; d.copy_from_slice(&f.data[..8]); d }).collect();
        m.can().tx.retain(|f| !(f.id == 0x7f8 && (0xc0..=0xc2).contains(&f.data[0])));
        a
    };
    let crc = {
        let mut c = 0xffff_ffffu32;
        for &b in &image {
            c ^= u32::from(b);
            for _ in 0..8 {
                c = (c >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(c & 1));
            }
        }
        !c
    };
    let n = image.len() as u32;
    let mut start = vec![1u8, (n >> 16) as u8, (n >> 8) as u8, n as u8];
    start.extend_from_slice(&crc.to_be_bytes());
    m.can_frame(Frame::new(0x7f0, &start));
    assert_eq!(m.run(50 * ms), Stop::Budget);
    let a = answers(&mut m);
    assert!(a.iter().any(|d| d[0] == 0xc0 && d[1] == 1), "{:?}: {}", a, console(&m));
    let frames = (image.len() + 6) / 7;
    let mut k = 0;
    while k < frames {
        let end = (k - k % 32 + 32).min(frames);
        for i in k..end {
            let mut d = vec![(i & 255) as u8];
            d.extend_from_slice(&image[7 * i..(7 * i + 7).min(image.len())]);
            m.can_frame(Frame::new(0x7f1, &d));
        }
        assert_eq!(m.run(60 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
        let a = answers(&mut m);
        let ack = a.iter().rev().find(|d| d[0] == 0xc1).unwrap_or_else(|| panic!("no answer at frame {}: {}", k, console(&m)));
        assert_eq!(ack[1], 0, "a frame missing at {}", k);
        let off = u32::from_be_bytes([ack[2], ack[3], ack[4], ack[5]]) as usize;
        k = if off >= image.len() { frames } else { (off + 6) / 7 };
    }
    let at = m.console.len();
    m.can_frame(Frame::new(0x7f0, &[3]));
    for _ in 0..30 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
        if String::from_utf8_lossy(&m.console[at..]).contains("rnse sdk: hello") {
            break;
        }
    }
    let text = String::from_utf8_lossy(&m.console[at..]).into_owned();
    assert!(text.contains("rnse sdk: hello"), "{}", text);
    assert_eq!(m.run(2_500 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    assert!(String::from_utf8_lossy(&m.console[at..]).contains("tick 2"), "{}", console(&m));
}

/// The phone's sound: a 1 kHz tone (48 kHz stereo, the left at half the
/// right's level) in the stream between the pictures, played by the
/// other card processor at 44.1 kHz through the amplifier's decoder
/// input, at its pitch and with no break while it lasts.
#[test]
fn viewer_plays_the_phone_s_sound() {
    use rnse::dev::radio::Radio;
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build() else { return };
    let Some(card) = card_with_sound(&root.join("tests/data/margin.264"), 3000) else { return };
    let mut m = start(&program, card);
    m.speaker = Some(Vec::new());
    let ms = m.cycles_per_ms();
    // The volume knob up to the top (the viewer starts at 12 of 30: the
    // emulated amplifier's gain follows it, 2 dB a step).
    for _ in 0..30 {
        if console(&m).contains("BRIDGE IN SLOT") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert!(m.panel_knob("volume", 18));
    for _ in 0..60 {
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    assert_eq!(m.radio.volume, 30, "the volume knob");
    let text = console(&m);
    println!("{}", text);
    assert!(text.contains("aaviewer: frame 11 "), "{}", text);
    assert_eq!(m.radio.source, Radio::SOURCE_DECODER, "{}", text);
    let heard = m.speaker.take().unwrap();
    let right: Vec<i16> = heard.iter().skip(1).step_by(2).copied().collect();
    let left: Vec<i16> = heard.iter().step_by(2).copied().collect();
    // Where the tone is: from its first loud sample, the 2.5 s after.
    let from = right.iter().position(|v| v.abs() > 8000).unwrap_or_else(|| panic!("no tone heard ({} samples): {}", right.len(), text));
    let tone = &right[from..(from + 110_250).min(right.len())];
    assert!(tone.len() >= 88_200, "only {} samples of the tone: {}", tone.len(), text);
    let pitches: Vec<u32> = tone.chunks(22_050).map(|c| 2 * c.windows(2).filter(|w| w[0] < 0 && w[1] >= 0).count() as u32).collect();
    for p in &pitches[..pitches.len() - 1] {
        assert!(p.abs_diff(1000) <= 8, "1 kHz each half second: {:?}", pitches);
    }
    // No break: no step between two samples beyond what the sine makes.
    let peak = tone.iter().map(|v| i32::from(*v).abs()).max().unwrap();
    let largest = tone.windows(2).map(|w| (i32::from(w[1]) - i32::from(w[0])).abs()).max().unwrap();
    let slope = (1.2 * 2.0 * std::f64::consts::PI * 1000.0 / 44_100.0 * f64::from(peak)) as i32;
    assert!(peak > 15_000 && largest <= slope + 2, "a step of {} where the sine of {} has at most {}", largest, peak, slope);
    // The left at half the right.
    let lpeak = left[from..from + tone.len()].iter().map(|v| i32::from(*v).abs()).max().unwrap();
    assert!((lpeak * 2 - peak).abs() < peak / 20, "left {} right {}", lpeak, peak);
}


/// The viewer on an emulated 193 where sdload193 would start it, the
/// bridge's card at select 0 (the unit's slot 2).
fn start_193(program: &[u8], card: Vec<u8>) -> Machine {
    let mut m = Machine::for_model(Model::Rnse193);
    m.echo_console = std::env::var_os("RNSE_LINK_ECHO").is_some();
    m.scramble_ram();
    m.install_boot_stub("V575");
    m.board.load(0x09b6_0080, program);
    m.cpu.reset();
    m.cpu.set_sr(0x7000_00f0);
    m.cpu.pc = u32::from_be_bytes([program[0], program[1], program[2], program[3]]);
    m.cpu.r[15] = 0x09c5_fff0;
    m.watchdog_always = true;
    assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    m
}

/// The 193: the same stream from the bridge's card in the slot its
/// software calls SD2 (the unit's physical slot 2), read through the
/// chip's SD host (CMD18), every picture decoded and shown whole; each
/// frame's CRC-32 as ffmpeg's.  The viewer is started in the loader's
/// place, as rnse-sdk's examples/sdload193 starts RNSE.BIN.
#[test]
fn viewer_decodes_the_bridge_s_stream_on_the_193() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let Some(card) = card(&root.join("tests/data/margin.264")) else { return };
    let want: Vec<String> = std::fs::read_to_string(root.join("tests/data/margin.crc")).unwrap()
        .lines().map(|l| l.trim().to_ascii_lowercase()).filter(|l| !l.is_empty()).collect();

    let mut m = start_193(&program, card);

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
    if let Some(path) = std::env::var_os("RNSE_AAVIEWER_SHOT_193") {
        let (w, h, rgb) = m.render();
        std::fs::write(path, rnse::png::encode_rgb(w, h, &rgb)).unwrap();
    }
    assert!(text.contains("BRIDGE IN SLOT 2"), "{}", text);
    let got = crcs(&m);
    assert_eq!(got.len(), want.len(), "{}", text);
    for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
        assert_eq!(g.trim_start_matches('0'), w.trim_start_matches('0'), "frame {}: {}", i, text);
    }
}

/// The keys on the 193: the knob and SELECT go to the phone as on the
/// 192, CD EJECT ejects the disc (the drive's port as the stock software
/// left it), and RETURN held says there is no loader to go back to and
/// the viewer goes on.
#[test]
fn viewer_keys_on_the_193() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let Some(program) = build_193() else { return };
    let Some(card) = card(&root.join("tests/data/margin.264")) else { return };
    let mut m = start_193(&program, card);
    let ms = m.cycles_per_ms();
    for _ in 0..200 {
        if console(&m).contains("aaviewer: frame 11 ") {
            break;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    }
    let at = m.console.len();
    assert!(m.panel_knob("main", 2));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    assert!(m.panel_key("SELECT", true));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    assert!(m.panel_key("SELECT", false));
    assert_eq!(m.run(300 * ms), Stop::Budget);
    let text = String::from_utf8_lossy(&m.console[at..]).into_owned();
    let what: Vec<u32> = text.lines().filter_map(|l| l.strip_prefix("aaviewer: input "))
        .map(|c| u32::from_str_radix(c.trim(), 16).unwrap() & 0x3ff).collect();
    let knob: i32 = what.iter().filter(|w| *w & 0x300 == 0x100).map(|w| (*w & 0xff) as u8 as i8 as i32).sum();
    let keys: Vec<u32> = what.iter().copied().filter(|w| w & 0x300 == 0).collect();
    assert_eq!(knob, 2, "{}", text);
    assert_eq!(keys, vec![6 << 1 | 1, 6 << 1], "{}", text);

    // The display key opens the display (the hardware rules: the disc
    // must be reachable from every program), the motor stops when it is
    // open, and the key closes it again.
    assert!(m.display_pos() >= 894, "starts closed: {}", m.display_pos());
    assert!(m.panel_key("EJECT", true));
    assert_eq!(m.run(200 * ms), Stop::Budget);
    assert!(m.panel_key("EJECT", false));
    assert_eq!(m.display_motor(), -1, "opening: {}", console(&m));
    assert_eq!(m.run(3_000 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    assert!(m.display_pos() <= 94, "open: {}", m.display_pos());
    assert_eq!(m.display_motor(), 0, "the motor stops when open");

    // A disc in the drive (any data: the eject is what is looked at).
    m.atapi().insert(Some(vec![0u8; 64 * 2048]));
    assert_eq!(m.run(500 * ms), Stop::Budget);
    let before = m.atapi().ejects;
    assert!(m.panel_key("CD_EJECT", true));
    assert_eq!(m.run(500 * ms), Stop::Budget);
    assert!(m.panel_key("CD_EJECT", false));
    assert_eq!(m.run(500 * ms), Stop::Budget);
    let ejects = m.atapi().ejects;
    assert_eq!(ejects, before + 1, "{}", console(&m));
    assert!(m.panel_key("EJECT", true));
    assert_eq!(m.run(200 * ms), Stop::Budget);
    assert!(m.panel_key("EJECT", false));
    assert_eq!(m.run(3_000 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    assert!(m.display_pos() >= 894, "closed again: {}", m.display_pos());
    assert_eq!(m.display_motor(), 0, "the motor stops when closed");

    assert!(m.panel_key("RETURN", true));
    assert_eq!(m.run(2500 * ms), Stop::Budget);
    assert!(m.panel_key("RETURN", false));
    assert_eq!(m.run(500 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(&m));
    assert!(console(&m).contains("RETURN: NO LOADER TO GO BACK TO"), "{}", console(&m));
    // Still reading: the report goes on.
    let at = m.console.len();
    assert_eq!(m.run(2500 * ms), Stop::Budget);
    assert!(String::from_utf8_lossy(&m.console[at..]).contains(" fps x10"), "{}", console(&m));
}
