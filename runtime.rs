use serde::Serialize;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VirtualCameraStatus {
    pub supported: bool,
    pub installed: bool,
    pub ready: bool,
    pub message: String,
}

#[cfg(windows)]
mod implementation {
    use super::VirtualCameraStatus;
    use crate::vcam::latest_frame_metadata_for_output;
    use std::{
        env,
        ffi::{c_void, OsStr},
        fs,
        os::windows::{ffi::OsStrExt, process::CommandExt},
        path::PathBuf,
        process::{Command, Stdio},
        ptr::NonNull,
        sync::OnceLock,
    };

    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    const DLL_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/MiniMixVcam.dll"));
    const REGISTRAR_BYTES: &[u8] =
        include_bytes!(concat!(env!("OUT_DIR"), "/MiniMixVcamRegistrar.exe"));

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn LoadLibraryW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }

    type PublishJpeg = unsafe extern "system" fn(u8, *const u8, u32) -> i32;
    type ClearFrame = unsafe extern "system" fn(u8) -> i32;

    struct NativePublisher {
        _module: NonNull<c_void>,
        publish_jpeg: PublishJpeg,
        clear: ClearFrame,
    }
    unsafe impl Send for NativePublisher {}
    unsafe impl Sync for NativePublisher {}

    static PUBLISHER: OnceLock<Result<NativePublisher, String>> = OnceLock::new();

    #[derive(Clone, Copy)]
    enum RegistrarStatus {
        Installed,
        NotInstalled,
        Unsupported,
    }

    fn wide(value: &str) -> Vec<u16> {
        OsStr::new(value).encode_wide().chain(Some(0)).collect()
    }

    fn runtime_directory() -> Result<PathBuf, String> {
        let base = env::var_os("LOCALAPPDATA")
            .map(PathBuf::from)
            .ok_or_else(|| "Hindi makita ang LOCALAPPDATA para sa MiniMix camera runtime.".to_string())?;
        Ok(base.join("MiniMix Pro").join("virtual-camera"))
    }

    fn write_if_changed(path: PathBuf, bytes: &[u8]) -> Result<PathBuf, String> {
        if fs::read(&path).ok().as_deref() != Some(bytes) {
            fs::write(&path, bytes)
                .map_err(|error| format!("Hindi maisulat ang {}: {error}", path.display()))?;
        }
        Ok(path)
    }

    fn ensure_runtime() -> Result<(PathBuf, PathBuf), String> {
        let directory = runtime_directory()?;
        fs::create_dir_all(&directory)
            .map_err(|error| format!("Hindi magawa ang MiniMix camera folder: {error}"))?;
        let dll = write_if_changed(directory.join("MiniMixVcam.dll"), DLL_BYTES)?;
        let registrar = write_if_changed(directory.join("MiniMixVcamRegistrar.exe"), REGISTRAR_BYTES)?;
        Ok((dll, registrar))
    }

    fn registrar_status(registrar: &PathBuf) -> Result<RegistrarStatus, String> {
        let status = Command::new(registrar)
            .arg("--status")
            .creation_flags(CREATE_NO_WINDOW)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map_err(|error| format!("Hindi masuri ang MiniMix camera registration: {error}"))?;
        match status.code() {
            Some(0) => Ok(RegistrarStatus::Installed),
            Some(4) => Ok(RegistrarStatus::NotInstalled),
            Some(6) => Ok(RegistrarStatus::Unsupported),
            Some(code) => Err(format!("Hindi masuri ang MiniMix camera registration (code {code}).")),
            None => Err("Hindi natapos ang MiniMix camera registration check.".into()),
        }
    }

    fn publisher() -> Result<&'static NativePublisher, String> {
        PUBLISHER
            .get_or_init(|| {
                let (dll, _) = ensure_runtime()?;
                let path = wide(&dll.to_string_lossy());
                let module = NonNull::new(unsafe { LoadLibraryW(path.as_ptr()) })
                    .ok_or_else(|| format!("Hindi ma-load ang {}.", dll.display()))?;
                let publish = unsafe { GetProcAddress(module.as_ptr(), b"minimix_vcam_publish_jpeg\0".as_ptr()) };
                let clear = unsafe { GetProcAddress(module.as_ptr(), b"minimix_vcam_clear\0".as_ptr()) };
                let Some(publish) = NonNull::new(publish) else {
                    return Err("Wala ang JPEG publisher sa MiniMix camera runtime.".to_string());
                };
                let Some(clear) = NonNull::new(clear) else {
                    return Err("Wala ang clear command sa MiniMix camera runtime.".to_string());
                };
                Ok(NativePublisher {
                    _module: module,
                    publish_jpeg: unsafe { std::mem::transmute(publish.as_ptr()) },
                    clear: unsafe { std::mem::transmute(clear.as_ptr()) },
                })
            })
            .as_ref()
            .map_err(Clone::clone)
    }

    pub(super) fn status() -> Result<VirtualCameraStatus, String> {
        let (_, registrar) = ensure_runtime()?;
        let registrar_status = registrar_status(&registrar)?;
        let (supported, installed) = match registrar_status {
            RegistrarStatus::Installed => (true, true),
            RegistrarStatus::NotInstalled => (true, false),
            RegistrarStatus::Unsupported => (false, false),
        };
        Ok(VirtualCameraStatus {
            supported,
            installed,
            ready: supported && installed,
            message: if !supported {
                "Hindi available ang Windows virtual-camera API. Kailangan ang Windows 11 22H2 o mas bago.".into()
            } else if installed {
                "Handa ang MiniMix 1–4 sa camera list ng Windows.".into()
            } else {
                "Hindi naka-install ang MiniMix camera. I-install muli gamit ang Windows installer at pahintulutan ang administrator setup.".into()
            },
        })
    }

    pub(super) fn publish_jpeg(output_id: u8, jpeg: Vec<u8>) -> Result<(), String> {
        if !(1..=4).contains(&output_id) || jpeg.is_empty() {
            return Err("Hindi valid ang MiniMix output frame.".into());
        }
        let jpeg_length = u32::try_from(jpeg.len())
            .map_err(|_| "Masyadong malaki ang MiniMix JPEG frame.".to_string())?;
        let previous_sequence = latest_frame_metadata_for_output(output_id)?
            .map(|metadata| metadata.sequence);
        let publisher = publisher()?;
        let result = unsafe { (publisher.publish_jpeg)(output_id, jpeg.as_ptr(), jpeg_length) };
        if result == 0 {
            let published_sequence = latest_frame_metadata_for_output(output_id)?
                .map(|metadata| metadata.sequence);
            if published_sequence.is_some_and(|sequence| {
                previous_sequence.map_or(sequence > 0, |previous| sequence > previous)
            }) {
                Ok(())
            } else {
                Err(format!(
                    "Nagtagumpay ang native publisher pero walang bagong frame sa shared-memory ring ng MiniMix {output_id}."
                ))
            }
        } else {
            Err(format!("Hindi maipadala ang video frame sa MiniMix {output_id} (native code {result})."))
        }
    }

    pub(super) fn clear(output_id: u8) -> Result<(), String> {
        if !(1..=4).contains(&output_id) {
            return Err("Hindi valid ang MiniMix output number.".into());
        }
        let publisher = publisher()?;
        let result = unsafe { (publisher.clear)(output_id) };
        if result == 0 {
            Ok(())
        } else {
            Err(format!("Hindi ma-clear ang MiniMix {output_id} (native code {result})."))
        }
    }
}

#[cfg(not(windows))]
mod implementation {
    use super::VirtualCameraStatus;

    fn unavailable() -> String {
        "Ang MiniMix virtual camera ay available lamang sa Windows 11 o mas bago.".into()
    }
    pub(super) fn status() -> Result<VirtualCameraStatus, String> {
        Ok(VirtualCameraStatus {
            supported: false,
            installed: false,
            ready: false,
            message: unavailable(),
        })
    }
    pub(super) fn publish_jpeg(_: u8, _: Vec<u8>) -> Result<(), String> { Err(unavailable()) }
    pub(super) fn clear(_: u8) -> Result<(), String> { Err(unavailable()) }
}

pub fn status() -> Result<VirtualCameraStatus, String> { implementation::status() }
pub fn publish_jpeg(output_id: u8, jpeg: Vec<u8>) -> Result<(), String> {
    implementation::publish_jpeg(output_id, jpeg)
}
pub fn clear(output_id: u8) -> Result<(), String> { implementation::clear(output_id) }
