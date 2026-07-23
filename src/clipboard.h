/*
 * clipboard.h — macOS clipboard image access.
 *
 * Pulls an image off the system pasteboard (via osascript, so no extra
 * link-time dependencies) and turns saved images into the base64 data
 * URLs the OpenRouter multimodal message format expects.
 */
#ifndef ORC_CLIPBOARD_H
#define ORC_CLIPBOARD_H

/*
 * If the clipboard currently holds an image, write it to `path` as PNG.
 * Returns 0 on success, 1 if the clipboard has no image (or it could
 * not be converted), -1 on internal error.
 */
int clip_image_save(const char *path);

/*
 * Read a PNG file and return it as a malloc'd
 * "data:image/png;base64,..." string, or NULL on error/OOM.
 */
char *clip_file_data_url(const char *path);

#endif /* ORC_CLIPBOARD_H */
