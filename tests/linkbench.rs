//! The SD link benchmark (`sdk/examples/linkbench`) on an emulated 192,
//! started in the loader's place by the boot ROM's stand-in: no firmware
//! needed, only an SH-4 GCC to build the program (the test returns at
//! once and passes without one).  Without the installed software the
//! program skips the stock way and the copying of the card processor's
//! pins; everything else, its own card processor program included, runs
//! as on a unit, with the caches modelled.
//!
//!     cargo test --release --test linkbench -- --nocapture
//!
//! What the emulator's figures do not say: the card answers at once and
//! the card interface's clock is taken to be the processor's 67.7 MHz
//! divided; only a unit has the real times (docs/android-auto.md).

use rnse::board::Model;
use rnse::machine::Machine;
use rnse::sh4::cpu::Stop;

/// The program built with -DBURST_PACE=8 (see `block_in` in its
/// source): the emulator's port reads take a cycle, a unit's bus several.
/// sdk/out/linkbench-192.bin is rebuilt without afterwards: that one is
/// for a unit.
fn build() -> Option<Vec<u8>> {
    static BUILT: std::sync::Mutex<Option<Option<Vec<u8>>>> = std::sync::Mutex::new(None);
    let mut built = BUILT.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(program) = built.as_ref() {
        return program.clone();
    }
    let sdk = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("sdk");
    let make = |extra: &str| std::process::Command::new("sh").arg(sdk.join("build.sh")).args(["192", "linkbench"])
        .env("EXTRA_CFLAGS", extra).output();
    let program = match make("-DBURST_PACE=8") {
        Ok(out) if out.status.success() => {
            let program = std::fs::read(sdk.join("out/linkbench-192.bin")).unwrap();
            assert!(make("").unwrap().status.success());
            Some(program)
        }
        Ok(out) => {
            let text = String::from_utf8_lossy(&out.stderr);
            // A compiler that is there and fails is a failure; none is a skip.
            assert!(!text.contains("error") || text.contains("not found"), "build: {}", text);
            None
        }
        Err(_) => None,
    };
    *built = Some(program.clone());
    program
}

/// The words of LINKTEST.BIN, as tools/linktest.py writes them.
fn pattern(j: u32) -> u32 {
    let mut x = j.wrapping_add(1).wrapping_mul(0x9e37_79b1);
    x ^= x >> 16;
    x = x.wrapping_mul(0x85eb_ca6b);
    x ^= x >> 13;
    x
}

fn linktest(bytes: usize) -> Vec<u8> {
    (0..bytes as u32 / 4).flat_map(|j| pattern(j).to_be_bytes()).collect()
}

fn start(program: &[u8], card: Option<Vec<u8>>) -> Machine {
    let mut m = Machine::for_model(Model::Rnse192);
    m.echo_console = std::env::var_os("RNSE_LINK_ECHO").is_some();
    m.scramble_ram();
    m.install_boot_stub("V222");
    m.board.load(0x0df0_0080, program);
    m.cpu.reset();
    m.cpu.set_sr(0x7000_00f0);
    m.cpu.pc = u32::from_be_bytes([program[0], program[1], program[2], program[3]]);
    m.cpu.r[15] = 0x8df1_fff0;
    m.busy_start();
    m.watchdog_always = true;
    m.cache_model();
    if let Some(card) = card {
        assert!(m.insert_card(0, rnse::dev::sdhi::Card::from_bytes(card).unwrap()));
    }
    m
}

fn console(m: &Machine) -> String {
    String::from_utf8_lossy(&m.console).into_owned()
}

/// Run until the console has `what`, for at most `seconds`.
fn until(m: &mut Machine, what: &str, seconds: u64) -> bool {
    let ms = m.cycles_per_ms();
    for _ in 0..seconds * 10 {
        if console(m).contains(what) {
            return true;
        }
        assert_eq!(m.run(100 * ms), Stop::Budget, "{:?}: {}", m.cpu.fault, console(m));
    }
    console(m).contains(what)
}

/// The summary's line for a test.
fn result<'a>(text: &'a str, name: &str) -> &'a str {
    text.lines().rev().find(|l| l.starts_with(name) && l.as_bytes().get(name.len()) == Some(&b' ') && l[name.len()..].trim_start().chars().next().is_some_and(|c| c.is_ascii_digit() || c == 'N'))
        .unwrap_or_else(|| panic!("no line for {}: {}", name, text))
}

const CARD_TESTS: [&str; 7] = ["CMD17 /4", "CMD18 /4", "CMD18 /3", "CMD18 /4 BURST", "CMD18 /3 BURST", "CMD18 /2", "CMD18 /2 BURST"];
const PORT_TESTS: [&str; 3] = ["PORT OUT", "PORT IN", "PORT OUT BURST"];

/// Every word as it should be, nothing late or missing.
fn clean(line: &str) {
    assert!(line.contains(" B0 T0 L0") && !line.contains("FAIL") && !line.contains(" S"), "{}", line);
}

#[test]
fn link_benchmark_reads_a_card_through_its_own_program() {
    let Some(program) = build() else { return };
    // A fresh card, so that the file is one run of clusters.
    let file = linktest(2 << 20);
    let card = rnse::fat::card("LINK", &[rnse::fat::Entry::file("LINKTEST.BIN", file)], None).unwrap();
    let mut m = start(&program, Some(card));
    assert!(until(&mut m, "rnse sdk: linkbench", 5), "{}", console(&m));
    assert!(until(&mut m, "DONE.", 300), "{}", console(&m));
    let text = console(&m);
    println!("{}", text);
    assert!(text.contains("CARD STARTED"), "{}", text);
    assert!(text.contains("LINKTEST.BIN: 4096 SECTORS"), "{}", text);
    assert!(result(&text, "STOCK DRIVER").contains("NOT RUN"));
    for name in CARD_TESTS.iter().chain(PORT_TESTS.iter()) {
        clean(result(&text, name));
    }
}

/// A word lost on the way shows: the same card with its file's words
/// changed in one sector.
#[test]
fn link_benchmark_sees_a_bad_sector() {
    let Some(program) = build() else { return };
    let mut file = linktest(2 << 20);
    file[512 * 100 + 7] ^= 1;
    let card = rnse::fat::card("LINK", &[rnse::fat::Entry::file("LINKTEST.BIN", file)], None).unwrap();
    let mut m = start(&program, Some(card));
    assert!(until(&mut m, "DONE.", 300), "{}", console(&m));
    let text = console(&m);
    for name in CARD_TESTS {
        assert!(result(&text, name).contains(" B1 T0"), "{}", text);
    }
    for name in PORT_TESTS {
        clean(result(&text, name));
    }
}

/// No card: the port tests run, the card tests do not, and the
/// program still ends.
#[test]
fn link_benchmark_without_a_card() {
    let Some(program) = build() else { return };
    let mut m = start(&program, None);
    assert!(until(&mut m, "DONE.", 300), "{}", console(&m));
    let text = console(&m);
    for name in PORT_TESTS {
        clean(result(&text, name));
    }
    for name in CARD_TESTS {
        assert!(result(&text, name).contains("NOT RUN"), "{}", text);
    }
}

/// The card the RP2040 bridge makes up (sdbridge/src/volume.c: a
/// partition table, FAT16, LINKTEST.BIN), written to an image by the
/// bridge's host test program, read by the benchmark: the FAT code and
/// the file's words as the unit will meet them.  Needs a host C
/// compiler as well.
#[test]
fn link_benchmark_reads_the_bridge_s_volume() {
    let Some(program) = build() else { return };
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let dir = std::env::temp_dir().join(format!("rnse-sdbridge-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let tool = dir.join("host");
    let src = root.join("sdbridge/src");
    let Ok(cc) = std::process::Command::new("cc").args(["-O2", "-o"]).arg(&tool).arg(root.join("sdbridge/tests/host.c"))
        .arg(src.join("card.c")).arg(src.join("volume.c")).arg(src.join("crc.c")).output() else { return };
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));
    let image = dir.join("card.img");
    assert!(std::process::Command::new(&tool).arg(&image).status().unwrap().success());
    let card = std::fs::read(&image).unwrap();
    std::fs::remove_dir_all(&dir).ok();
    assert_eq!(card.len(), 64 << 20);
    let mut m = start(&program, Some(card));
    assert!(until(&mut m, "DONE.", 300), "{}", console(&m));
    let text = console(&m);
    assert!(text.contains("CARD STARTED"), "{}", text);
    assert!(text.contains("LINKTEST.BIN: 4096 SECTORS"), "{}", text);
    for name in CARD_TESTS.iter().chain(PORT_TESTS.iter()) {
        clean(result(&text, name));
    }
}
