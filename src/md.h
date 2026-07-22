/*
 * md.h — Markdown → ANSI terminal renderer.
 *
 * Parses a complete Markdown document with libcmark and renders it to
 * stdout with ANSI styling (colored headings, bold/italic, code blocks,
 * GFM-style tables, lists, blockquotes, links, and rules). Rendering is
 * meant for a finished assistant reply, not partial streaming text.
 */
#ifndef ORC_MD_H
#define ORC_MD_H

/* True if stdout is a terminal that should get colored Markdown. */
int md_stdout_is_tty(void);

/* True when ANSI colors are appropriate (also honors NO_COLOR). */
int md_color_enabled(void);

/*
 * Render Markdown `text` to stdout. When `color` is nonzero ANSI escape
 * sequences are emitted; otherwise the output is plain text. A trailing
 * newline is always written. Never fails destructively: on parser/OOM
 * trouble it falls back to printing the raw text.
 */
void md_render(const char *text, int color);

#endif /* ORC_MD_H */
