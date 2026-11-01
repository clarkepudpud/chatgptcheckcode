mod shared_memory;
mod runtime;

pub use shared_memory::{
    FrameMetadata, FrameSnapshot, latest_frame, latest_frame_metadata,
    latest_frame_metadata_for_output,
};
pub use runtime::{VirtualCameraStatus};

#[tauri::command]
pub fn get_vcam_status() -> Result<Option<FrameMetadata>, String> {
    latest_frame_metadata()
}

#[tauri::command]
pub fn get_virtual_camera_status() -> Result<VirtualCameraStatus, String> {
    runtime::status()
}

#[tauri::command]
pub fn publish_virtual_camera_jpeg(output_id: u8, jpeg: Vec<u8>) -> Result<(), String> {
    runtime::publish_jpeg(output_id, jpeg)
}

#[tauri::command]
pub fn clear_virtual_camera_frame(output_id: u8) -> Result<(), String> {
    runtime::clear(output_id)
}
