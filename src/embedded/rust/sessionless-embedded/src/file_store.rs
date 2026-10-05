use crate::{KeyStore, PRIVATE_KEY_LEN};
use std::io::{ErrorKind, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;

/// Host-side store: the private key as 64 hex chars in a 0600 file.
/// For host tools and tests; microcontrollers implement `KeyStore` over NVS/flash.
pub struct FileStore {
    path: PathBuf,
}

impl FileStore {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }
}

impl KeyStore for FileStore {
    type Error = std::io::Error;

    fn load(&mut self, out: &mut [u8; PRIVATE_KEY_LEN]) -> Result<Option<usize>, Self::Error> {
        let mut text = String::new();
        match std::fs::File::open(&self.path) {
            Ok(mut f) => f.read_to_string(&mut text)?,
            Err(e) if e.kind() == ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        let text = text.trim();
        // Anything that isn't exactly 64 hex chars is reported as a wrong-length key, i.e. corrupt.
        if crate::from_hex(text, out).is_some() {
            Ok(Some(PRIVATE_KEY_LEN))
        } else {
            Ok(Some(0))
        }
    }

    fn save(&mut self, key: &[u8; PRIVATE_KEY_LEN]) -> Result<(), Self::Error> {
        let hex: [u8; PRIVATE_KEY_LEN * 2] = crate::to_hex(key);
        // create_new: never clobber an existing file.
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&self.path)?;
        f.write_all(&hex)?;
        f.sync_all()
    }
}
