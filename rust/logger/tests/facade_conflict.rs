//! A separate process is required because log's global receiver is set once.
use std::sync::atomic::{AtomicUsize, Ordering};

struct ExistingLogger;
static EXISTING: ExistingLogger = ExistingLogger;
static RECEIVED: AtomicUsize = AtomicUsize::new(0);

impl log::Log for ExistingLogger {
    fn enabled(&self, _: &log::Metadata<'_>) -> bool {
        true
    }
    fn log(&self, _: &log::Record<'_>) {
        RECEIVED.fetch_add(1, Ordering::Relaxed);
    }
    fn flush(&self) {}
}

#[test]
fn initialization_fails_without_replacing_an_existing_receiver() {
    log::set_logger(&EXISTING).unwrap();
    log::set_max_level(log::LevelFilter::Trace);
    assert_eq!(logger::start(), -1);
    assert_eq!(logger::option_value(logger::INITIALIZED, 0, 0), 0);
    log::info!("the pre-existing receiver remains installed");
    assert_eq!(RECEIVED.load(Ordering::Relaxed), 1);
    assert_eq!(logger::shutdown(), 0);
}
