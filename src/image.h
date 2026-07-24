/*
 * image.h — save and render model-generated images.
 *
 * Image-output models return pictures in the reply as data: URLs
 * (data:<mime>;base64,<payload>). This module base64-decodes them to
 * files and, on terminals that speak the Kitty graphics protocol
 * (kitty, Ghostty, WezTerm), renders them inline. orc's text pipeline is
 * unaffected — this only handles the image parts the API returns.
 */
#ifndef ORC_IMAGE_H
#define ORC_IMAGE_H

/*
 * Decode a data: URL and write the bytes to
 * <dir>/img-<ts>-<index>.<ext>, the extension taken from the URL's mime
 * subtype (png, jpeg, …). Returns a malloc'd path (caller frees) or NULL
 * if the URL is not a base64 data URL or the write failed. `dir` must
 * already exist.
 */
char *img_save_data_url(const char *data_url, const char *dir,
                        long ts, int index);

/* True if the terminal appears to support the Kitty graphics protocol. */
bool img_terminal_supports_graphics(void);

/*
 * Render a data: URL inline via the Kitty graphics protocol. Only PNG is
 * supported (transmit format f=100); returns false for other formats, on
 * an unsupported terminal, or on malformed input, so the caller can fall
 * back to printing the saved path.
 */
bool img_render_kitty(const char *data_url);

#endif /* ORC_IMAGE_H */
