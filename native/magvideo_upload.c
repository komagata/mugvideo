#include "magvideo_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str_arg(TyaValue value) {
  if (value.kind == TYA_STRING && value.string != NULL) return value.string;
  return "";
}

TyaValue tya_magvideo_upload_put(TyaValue __this, TyaValue file, TyaValue destination, TyaValue key, TyaValue content_type) {
  (void)__this; (void)file; (void)content_type;
  const char *dest = str_arg(destination);
  const char *object_key = str_arg(key);
  TyaValue result = tya_dict(NULL, 0);
  if (dest[0] == '\0') {
    tya_dict_set(result, tya_string("ok"), tya_bool(false));
    tya_dict_set(result, tya_string("error"), tya_string("upload destination is not configured"));
    return result;
  }
  int len = snprintf(NULL, 0, "%s/%s", dest, object_key);
  char *url = (char *)malloc((size_t)len + 1);
  snprintf(url, (size_t)len + 1, "%s/%s", dest, object_key);
  tya_dict_set(result, tya_string("ok"), tya_bool(true));
  tya_dict_set(result, tya_string("url"), tya_string(url));
  return result;
}
