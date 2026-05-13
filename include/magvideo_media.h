#ifndef MAGVIDEO_MEDIA_H
#define MAGVIDEO_MEDIA_H

#include "tya_runtime.h"

TyaValue tya_magvideo_media_init(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_devices(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_start_preview(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_stop_preview(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_start_recording(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_stop_recording(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_elapsed(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_last_error(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_media_open_file(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_clipboard_set_file(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_config_dir(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);
TyaValue tya_magvideo_output_dir(TyaValue, TyaValue, TyaValue, TyaValue, TyaValue);

#endif
