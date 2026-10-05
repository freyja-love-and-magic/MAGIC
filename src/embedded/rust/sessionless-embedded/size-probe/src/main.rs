//! Smallest useful no_std program: load-or-create, sign, verify.
//! Built only to measure code size on bare-metal targets (see README); not runnable as-is.
#![no_std]
#![no_main]

use core::panic::PanicInfo;
use rand_core::{CryptoRng, Error, RngCore};
use sessionless_embedded::{verify, Identity, MemoryStore};

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {}
}

// Reads a memory-mapped register, so the optimizer cannot see through it (like a real TRNG).
// The address is arbitrary: this binary is only linked to measure size, never run.
struct HwRng;
fn reg() -> u32 { unsafe { core::ptr::read_volatile(0x6003_5144 as *const u32) } }
impl RngCore for HwRng {
    fn next_u32(&mut self) -> u32 { reg() }
    fn next_u64(&mut self) -> u64 { (reg() as u64) << 32 | reg() as u64 }
    fn fill_bytes(&mut self, dest: &mut [u8]) { for b in dest { *b = reg() as u8; } }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), Error> { self.fill_bytes(dest); Ok(()) }
}
impl CryptoRng for HwRng {}

#[no_mangle]
pub extern "C" fn entry() -> u32 {
    let mut store = MemoryStore::new();
    let Ok(id) = Identity::load_or_create(&mut store, &mut HwRng) else { return 1 };
    let sig = id.sign(b"hello");
    verify(&sig, b"hello", id.public_key()) as u32
}
