#include <yed/plugin.h>

static yed_plugin *Self;

static void unload(yed_plugin *self);

static void fstyle(int n_args, char **args);
static void fstyle_export(int n_args, char **args);
static void fstyle_from(int n_args, char **args);

static void maybe_change_ft(yed_buffer *buff);
static void maybe_change_ft_event(yed_event *event);
static void syntax_fstyle_line_handler(yed_event *event);
static void syntax_fstyle_row_handler(yed_event *event);

static yed_attrs parse_attr_line(char *line, int *scomp);

static const char *scomp_names[] = {
    "", /* NO_SCOMP */
    #define __SCOMP(comp) #comp,
    __STYLE_COMPONENTS
    #undef __SCOMP
};

int yed_plugin_boot(yed_plugin *self) {
    int                                          i;
    tree_it(yed_buffer_name_t, yed_buffer_ptr_t) bit;
    yed_event_handler                            buff_post_load_handler;
    yed_event_handler                            buff_pre_write_handler;
    yed_event_handler                            line_handler;
    yed_event_handler                            row_handler;

    YED_PLUG_VERSION_CHECK();

    Self = self;

    yed_plugin_set_unload_fn(self, unload);

    if (yed_plugin_make_ft(self, "fstyle") == FT_ERR_TAKEN) {
        LOG_CMD_ENTER("fstyle");
        yed_cerr("lang/yedrc: unable to create file type name");
        LOG_EXIT();
        return 1;
    }

    yed_plugin_set_command(self, "fstyle",        fstyle);
    yed_plugin_set_command(self, "fstyle-export", fstyle_export);
    yed_plugin_set_command(self, "fstyle-from",   fstyle_from);

    yed_plugin_set_completion(self, "fstyle-compl-arg-0",        yed_get_completion("file"));
    yed_plugin_set_completion(self, "fstyle-from-compl-arg-0",   yed_get_completion("style"));
    yed_plugin_set_completion(self, "fstyle-from-compl-arg-1",   yed_get_completion("file"));

    buff_post_load_handler.kind = EVENT_BUFFER_POST_LOAD;
    buff_post_load_handler.fn   = maybe_change_ft_event;
    buff_pre_write_handler.kind = EVENT_BUFFER_PRE_WRITE;
    buff_pre_write_handler.fn   = maybe_change_ft_event;
    line_handler.kind           = EVENT_LINE_PRE_DRAW;
    line_handler.fn             = syntax_fstyle_line_handler;
    row_handler.kind            = EVENT_ROW_PRE_CLEAR;
    row_handler.fn              = syntax_fstyle_row_handler;

    yed_plugin_add_event_handler(self, buff_post_load_handler);
    yed_plugin_add_event_handler(self, buff_pre_write_handler);
    yed_plugin_add_event_handler(self, line_handler);
    yed_plugin_add_event_handler(self, row_handler);

    tree_traverse(ys->buffers, bit) {
        maybe_change_ft(tree_it_val(bit));
    }

    return 0;
}

static void unload(yed_plugin *self) {}

static void fstyle(int n_args, char **args) {
    char       *path;
    char        full_path[4096];
    FILE       *f;
    const char *base_ext;
    const char *base;
    yed_style   s;
    char        line[512];
    yed_attrs   attr;
    int         scomp;

    if (n_args == 0) {
        if (ys->active_frame == NULL) {
            yed_cerr("no active frame");
            return;
        }
        if (ys->active_frame->buffer == NULL) {
            yed_cerr("active frame has no buffer");
            return;
        }

        if (ys->active_frame->buffer->ft != yed_get_ft("fstyle")) {
            yed_cerr("buffer's ft is not 'fstyle'");
            return;
        }

        path = ys->active_frame->buffer->path;
        if (path == NULL) {
            yed_cerr("buffer has not been written");
            return;
        }
    } else if (n_args == 1) {
        path = args[0];
    } else {
        yed_cerr("expected 0 or 1 arguments, but got %d", n_args);
        return;
    }

    abs_path(path, full_path);

    f = fopen(full_path, "r");
    if (f == NULL) {
        yed_cerr("unable to open '%s'", full_path);
        return;
    }

    base_ext = get_path_basename(path);
    base     = path_without_ext(base_ext);

    memset(&s, 0, sizeof(s));

    while (fgets(line, sizeof(line), f)) {
        if (*line && line[strlen(line) - 1] == '\n') { line[strlen(line) - 1] = 0; }
        if (strlen(line) == 0) { continue; }

        attr = parse_attr_line(line, &scomp);

        switch (scomp) {
            #define __SCOMP(comp) case STYLE_##comp: s.comp = attr; break;
            __STYLE_COMPONENTS
            #undef __SCOMP
        }
    }

    yed_plugin_set_style(Self, (char*)base, &s);
    YEXE("style", (char*)base);

    free((void*)base);

    fclose(f);
}

static void flags_str(int flags, char *buff) {
    buff[0] = 0;

    strcat(buff, "0");
    if (flags & ATTR_INVERSE)     { strcat(buff, " | ATTR_INVERSE");     }
    if (flags & ATTR_BOLD)        { strcat(buff, " | ATTR_BOLD");        }
    if (flags & ATTR_UNDERLINE)   { strcat(buff, " | ATTR_UNDERLINE");   }
    if (flags & ATTR_16_LIGHT_FG) { strcat(buff, " | ATTR_16_LIGHT_FG"); }
    if (flags & ATTR_16_LIGHT_BG) { strcat(buff, " | ATTR_16_LIGHT_BG"); }
}

static void put_attrs(FILE *f, const char *scomp_name, yed_attrs attrs) {
    int  is_16;
    char buff[512];

    if (ATTR_FG_KIND(attrs.flags) != ATTR_KIND_NONE) {
        is_16 = ATTR_FG_KIND(attrs.flags) == ATTR_KIND_16;
        fprintf(f, "    ATTR_SET_FG_KIND(s.%s.flags, %s);\n", scomp_name, is_16 ? "ATTR_KIND_16" : "attr_kind");
        fprintf(f, "    s.%s.fg     = %s0x%x%s;\n", scomp_name, is_16 ? "" : "MAYBE_CONVERT(", attrs.fg, is_16 ? "" : ")");
    }
    if (ATTR_BG_KIND(attrs.flags) != ATTR_KIND_NONE) {
        is_16 = ATTR_BG_KIND(attrs.flags) == ATTR_KIND_16;
        fprintf(f, "    ATTR_SET_BG_KIND(s.%s.flags, %s);\n", scomp_name, is_16 ? "ATTR_KIND_16" : "attr_kind");
        fprintf(f, "    s.%s.bg     = %s0x%x%s;\n", scomp_name, is_16 ? "" : "MAYBE_CONVERT(", attrs.bg, is_16 ? "" : ")");
    }
    if (attrs.flags >> 4) {
        flags_str(attrs.flags, buff);
        fprintf(f, "    s.%s.flags |= %s;\n", scomp_name, buff);
    }
    fprintf(f, "\n");
}

static void fstyle_export(int n_args, char **args) {
    yed_buffer *buffer;
    char       *name;
    char        buff[4096];
    FILE       *f;
    yed_line   *line;
    yed_attrs   attrs;
    int         scomp;

    if (n_args != 0) {
        yed_cerr("expected 0 arguments, but got %d", n_args);
        return;
    }

    if (ys->active_frame == NULL) {
        yed_cerr("no active frame");
        return;
    }
    if (ys->active_frame->buffer == NULL) {
        yed_cerr("active frame has no buffer");
        return;
    }

    if (ys->active_frame->buffer->ft != yed_get_ft("fstyle")) {
        yed_cerr("buffer's ft is not 'fstyle'");
        return;
    }

    buffer = ys->active_frame->buffer;

    name = path_without_ext(get_path_basename(buffer->name));
    snprintf(buff, sizeof(buff), "%s.c", name);

    f = fopen(buff, "w");
    if (f == NULL) {
        yed_cerr("error opening '%s' for writing", buff);
        goto out_free;
    }

    fprintf(f, "#include <yed/plugin.h>\n");
    fprintf(f, "\n");
    fprintf(f, "#define MAYBE_CONVERT(rgb) (tc ? (rgb) : rgb_to_256(rgb))\n");
    fprintf(f, "\n");
    fprintf(f, "PACKABLE_STYLE(%s) {\n", name);
    fprintf(f, "    yed_style s;\n");
    fprintf(f, "    int       tc,\n");
    fprintf(f, "              attr_kind;\n");
    fprintf(f, "\n");
    fprintf(f, "    YED_PLUG_VERSION_CHECK();\n");
    fprintf(f, "\n");
    fprintf(f, "    tc        = !!yed_get_var(\"truecolor\");\n");
    fprintf(f, "    attr_kind = tc ? ATTR_KIND_RGB : ATTR_KIND_256;\n");
    fprintf(f, "\n");
    fprintf(f, "    memset(&s, 0, sizeof(s));\n");

    bucket_array_traverse(buffer->lines, line) {
        array_zero_term(line->chars);
        scomp = -1;

        attrs = parse_attr_line(line->chars.data, &scomp);
        if (scomp == -1) { continue; }

        switch (scomp) {
            #define __SCOMP(comp) case STYLE_##comp: put_attrs(f, #comp, attrs); break;
            __STYLE_COMPONENTS
            #undef __SCOMP
        }
    }

    fprintf(f, "    yed_plugin_set_style(self, \"%s\", &s);\n", name);
    fprintf(f, "    return 0;\n");
    fprintf(f, "}\n");

    fclose(f);

    yed_cprint("exported to '%s'", buff);

out_free:;
    free(name);
}

static void fstyle_from(int n_args, char **args) {
    yed_style *style;
    char       buff[4096];
    FILE      *f;
    int        s;
    int        i;
    yed_attrs  attrs;

    if (n_args < 1 || n_args > 2) {
        yed_cerr("expected 1 or 2 arguments, but got %d", n_args);
        return;
    }

    if ((style = yed_get_style(args[0])) == NULL) {
        yed_cerr("unknown style '%s'", args[0]);
        return;
    }

    if (n_args == 2) {
        snprintf(buff, sizeof(buff), "%s", args[1]);
    } else {
        snprintf(buff, sizeof(buff), "%s.fstyle", args[0]);
    }

    if ((f = fopen(buff, "w")) == NULL) {
        yed_cerr("unable to open file '%s' for writing", buff);
        return;
    }

    fprintf(f, "# fstyle generated for '%s'\n", args[0]);

    for (s = NO_SCOMP + 1; s < N_SCOMPS; s += 1) {
        switch (s) {
        case STYLE_white:
        case STYLE_gray:
        case STYLE_black:
        case STYLE_red:
        case STYLE_orange:
        case STYLE_yellow:
        case STYLE_lime:
        case STYLE_green:
        case STYLE_turquoise:
        case STYLE_cyan:
        case STYLE_blue:
        case STYLE_purple:
        case STYLE_magenta:
        case STYLE_pink:
            break;
        default:

            snprintf(buff, sizeof(buff), "%s", scomp_names[s]);
            for (i = 0; i < strlen(buff); i += 1) {
                if (buff[i] == '_') { buff[i] = '-'; }
            }
            fprintf(f, "%-20s ", buff);

            attrs = yed_get_style_scomp(style, s);

            if (ATTR_FG_KIND(attrs.flags)) {
                fprintf(f, "fg ");
                switch (ATTR_FG_KIND(attrs.flags)) {
                    case ATTR_KIND_16:
                        fprintf(f, "!%u", attrs.fg - ATTR_16_BLACK);
                        if (attrs.flags & ATTR_16_LIGHT_FG) {
                            fprintf(f, "  16-light-fg");
                        }
                        break;
                    case ATTR_KIND_256:
                        fprintf(f, "@%u", attrs.fg);
                        break;
                    case ATTR_KIND_RGB:
                        fprintf(f, "%x", attrs.fg);
                        break;
                }
                fprintf(f, "  ");
            }

            if (ATTR_BG_KIND(attrs.flags)) {
                fprintf(f, "bg ");
                switch (ATTR_BG_KIND(attrs.flags)) {
                    case ATTR_KIND_16:
                        fprintf(f, "!%u", attrs.bg - ATTR_16_BLACK);
                        if (attrs.flags & ATTR_16_LIGHT_BG) {
                            fprintf(f, "  16-light-bg");
                        }
                        break;
                    case ATTR_KIND_256:
                        fprintf(f, "@%u", attrs.bg);
                        break;
                    case ATTR_KIND_RGB:
                        fprintf(f, "%x", attrs.bg);
                        break;
                }
            }

            if (attrs.flags & ATTR_INVERSE) {
                fprintf(f, "  inverse");
            }

            if (attrs.flags & ATTR_BOLD) {
                fprintf(f, "  bold");
            }

            if (attrs.flags & ATTR_UNDERLINE) {
                fprintf(f, "  underline");
            }

            fprintf(f, "\n");
        }
    }

    fclose(f);
}

static void maybe_change_ft(yed_buffer *buff) {
    const char *ext;

    if (buff->ft != FT_UNKNOWN) {
        return;
    }
    if (buff->path == NULL) {
        return;
    }
    if ((ext = get_path_ext(buff->path)) != NULL) {
        if (strcmp(ext, "fstyle") == 0) {
            yed_buffer_set_ft(buff, yed_get_ft("fstyle"));
        }
    }
}

static void maybe_change_ft_event(yed_event *event) {
    if (event->buffer) {
        maybe_change_ft(event->buffer);
    }
}


static yed_attrs known_active;

static void syntax_fstyle_line_handler(yed_event *event) {
    yed_frame *frame;
    yed_line  *line;
    yed_attrs  attrs;
    yed_attrs *ait;

    frame = event->frame;

    if (!frame
    ||  !frame->buffer
    ||  frame->buffer->kind != BUFF_KIND_FILE
    ||  frame->buffer->ft != yed_get_ft("fstyle")) {
        return;
    }

    line = yed_buff_get_line(frame->buffer, event->row);
    if (line == NULL) { return; }

    array_zero_term(line->chars);
    attrs = parse_attr_line(array_data(line->chars), NULL);

    if (attrs.flags != 0) {
        array_traverse(event->eline_attrs, ait) {
            *ait = known_active;
            yed_combine_attrs(ait, &attrs);
        }
    }
}

static void syntax_fstyle_row_handler(yed_event *event) {
    yed_frame *frame;
    yed_line  *line;
    int        scomp;
    yed_attrs  attrs;

    frame = event->frame;

    if (!frame
    ||  !frame->buffer
    ||  frame->buffer->kind != BUFF_KIND_FILE
    ||  frame->buffer->ft != yed_get_ft("fstyle")) {
        return;
    }

    line = yed_buff_get_line(frame->buffer, event->row);
    if (line == NULL) { return; }

    scomp = -1;

    array_zero_term(line->chars);
    attrs = parse_attr_line(array_data(line->chars), &scomp);

    if (scomp == STYLE_active) {
        if (memcmp(&attrs, &known_active, sizeof(attrs))) {
            known_active = attrs;
        }
    }

    event->row_base_attr = known_active;
    if (attrs.flags != 0) {
        yed_combine_attrs(&event->row_base_attr, &attrs);
    }
}

static yed_attrs parse_attr_line(char *line, int *scomp) {
    char      *scomp_str;
    yed_attrs  attrs;
    array_t    words;
    int        idx;
    char      *word;
    unsigned   color;
    char       rgb_str[9];

    memset(&attrs, 0, sizeof(attrs));

    words = sh_split(line);

    if (array_len(words) == 0) { goto out; }

    scomp_str = *(char**)array_item(words, 0);
    if (scomp != NULL) {
        *scomp = yed_scomp_nr_by_name(scomp_str);
    }

    if (array_len(words) < 2) { goto out; }

    attrs = yed_parse_attrs(line + strlen(scomp_str));

out:;
    free_string_array(words);
    return attrs;
}
