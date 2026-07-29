#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libnotify/notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

/* Hardware interface definitions */
#define GPU_ACTUAL  "/sys/class/drm/card0/gt_act_freq_mhz"
#define GPU_MAX     "/sys/class/drm/card0/gt_max_freq_mhz"
#define GPU_BOOST   "/sys/class/drm/card0/gt_boost_freq_mhz"
#define CPU_MAX_PCT "/sys/devices/system/cpu/intel_pstate/max_perf_pct"

/* Timing and geometry parameters */
#define UPDATE_INTERVAL_MS 1750
#define COOLDOWN_TICKS_REQ 4
#define POPUP_OFFSET       4
#define MAX_POPUP_SENSORS  16

/* Global file descriptor tracking indices */
enum { 
    FD_STAT, 
    FD_MEM, 
    FD_CPUF, 
    FD_GPUF, 
    FD_TEMP, 
    FD_FAN, 
    FD_BATC, 
    FD_BATS, 
    FD_COUNT 
};

/* Color-level CSS classes for dynamic UI theme */
static const char * const LVL_CLASSES[] = { 
    "sysmon-lvl-0", 
    "sysmon-lvl-1", 
    "sysmon-lvl-2", 
    "sysmon-lvl-3", 
    "sysmon-lvl-4" 
};

static const char * const ALL_DYN_CLASSES[] = { 
    "sysmon-lvl-0", 
    "sysmon-lvl-1", 
    "sysmon-lvl-2", 
    "sysmon-lvl-3", 
    "sysmon-lvl-4", 
    "sysmon-static-c5", 
    "sysmon-sep", 
    NULL 
};

/* Core data structures for tracking panel labels and popup sensors */
typedef struct { 
    GtkWidget *widget; 
    char cur_class[32];
    char cur_text[16]; 
} SysMonLabel;

typedef struct { 
    GtkWidget *l_val; 
    char path[64]; 
    int fd; 
    char cur_class[32];
    char cur_text[16]; 
} PopupSensorRef;

typedef struct {
    XfcePanelPlugin    *plugin;
    GtkWidget          *popup;
    NotifyNotification *n_throttle;
    NotifyNotification *n_power;
    SysMonLabel         l_cpu_u, l_cpu_f, l_temp, l_sep, l_gpu_f, l_fan, l_mem, l_bat;
    PopupSensorRef      popup_sensors[MAX_POPUP_SENSORS];
    unsigned long long  pi, pt;
    gint64              last_hide_time;
    int                 throttle_lvl;
    int                 cool_ticks;
    int                 f_ac;
    int                 f_b15;
    int                 popup_sensor_count;
    guint               popup_timer_id;
    guint               main_timer_id;
    int                 fds[FD_COUNT];
    char                f_notify[32];
    gboolean            notify_initialized;
} SysMonPlugin;

static int notify_ref_count = 0;

/**
 * Returns a color class index based on defined value thresholds.
 */
static inline int get_level(long val, long v1, long v2, long v3, long v4) { 
    return (val < v1) ? 0 : (val < v2) ? 1 : (val < v3) ? 2 : (val < v4) ? 3 : 4; 
}

/**
 * Fast, non-blocking numeric parser designed for virtual sysfs integers.
 */
static inline unsigned long long quick_parse_ull(char **ptr) {
    char *p = *ptr; 
    while (*p && (*p < '0' || *p > '9')) {
        p++;
    }
    
    unsigned long long val = 0; 
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (unsigned long long)(*p - '0');
        p++;
    }
    
    *ptr = p; 
    return val;
}

/**
 * Reads and parses an integer from a persistent open file descriptor.
 */
static long read_sys_fd(int fd) {
    if (fd < 0) return 0;
    
    char buf[32] = {0};
    char *p = buf; 
    
    return (pread(fd, buf, sizeof(buf) - 1, 0) > 0) ? (long)quick_parse_ull(&p) : 0;
}

/**
 * Reads a single string from the filesystem and removes whitespace trailing tags.
 */
static void read_sys_str(const char *path, char *buf, size_t sz) {
    buf[0] = '\0'; 
    int fd = open(path, O_RDONLY);
    
    if (fd >= 0) { 
        ssize_t n = read(fd, buf, sz - 1); 
        close(fd); 
        if (n > 0) { 
            buf[n] = '\0'; 
            buf[strcspn(buf, "\r\n")] = '\0'; 
        } 
    }
}

/**
 * Safely writes a configuration value to a target path without unused-result warnings.
 */
static void write_sys(const char *path, const char *val) { 
    int fd = open(path, O_WRONLY); 
    if (fd >= 0) { 
        ssize_t written = write(fd, val, strlen(val)); 
        (void)written; 
        close(fd); 
    } 
}

/**
 * Sends a desktop notification using the libnotify framework if initialized.
 */
static void send_state_notify(NotifyNotification **n, const char *msg, int is_crit, gboolean notify_ok) {
    if (!notify_ok) return;
    
    if (*n == NULL) {
        *n = notify_notification_new(msg, NULL, NULL);
    } else {
        notify_notification_update(*n, msg, NULL, NULL);
    }
    
    notify_notification_set_timeout(*n, 5000); 
    notify_notification_set_urgency(*n, is_crit ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL);
    notify_notification_show(*n, NULL);
}

/**
 * Compiles and attaches custom CSS overrides once globally per session.
 */
static void setup_css(void) {
    static gboolean css_loaded = FALSE; 
    if (css_loaded) return;    
    
    const char css[] =
        ".sysmon-lvl-0, .sysmon-lvl-1, .sysmon-lvl-2, .sysmon-lvl-3, .sysmon-lvl-4, "
        ".sysmon-ncharge, .sysmon-static-c5, .sysmon-lvl-1-charging { transition: color 0.8s ease-out; }"
        ".sysmon-lvl-0{color:#6272a4;} .sysmon-lvl-1{color:#50fa7b;} .sysmon-lvl-2{color:#f1fa8c;}"
        ".sysmon-lvl-3{color:#ffb86c;} .sysmon-lvl-4{color:#ff5555;} .sysmon-lvl-1-charging{color:#50fa7b;}"
        ".sysmon-ncharge{color:#99d1db;} .sysmon-static-c5{color:#a6adc8;} .sysmon-sep{color:#343746;}"
        "window.sysmon-popup-window{background:transparent;}"
        ".sysmon-popup-container{background:#1e1f29;border:1px solid #44475a;border-radius:8px;padding:16px;}"
        ".sysmon-popup-container separator.horizontal{background:#44475a;min-height:1px;margin:6px 0;}"
        ".sysmon-popup-container separator.vertical{background:#44475a;min-width:1px;margin:0 12px;}";
        
    css_loaded = TRUE;
    GtkCssProvider *p = gtk_css_provider_new(); 
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); 
    g_object_unref(p);
}

/**
 * Initializes tracking structures for panel label widgets.
 */
static void sl_init(SysMonLabel *sl) { 
    sl->widget = gtk_label_new(NULL); 
    sl->cur_class[0] = sl->cur_text[0] = '\0'; 
    gtk_style_context_add_class(gtk_widget_get_style_context(sl->widget), "sysmon-label"); 
}

/**
 * Updates a panel label with styling changes only when values transition.
 */
static void set_label(SysMonLabel *sl, const char *text, const char *cls) {
    const char *safe = cls ? cls : ""; 
    if (!strcmp(sl->cur_text, text) && !strcmp(sl->cur_class, safe)) return;
    
    if (strcmp(sl->cur_text, text)) { 
        gtk_label_set_text(GTK_LABEL(sl->widget), text); 
        g_strlcpy(sl->cur_text, text, sizeof(sl->cur_text)); 
    }
    
    if (strcmp(sl->cur_class, safe)) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(sl->widget); 
        if (sl->cur_class[0]) {
            gtk_style_context_remove_class(ctx, sl->cur_class);
        }
        if (cls) {
            gtk_style_context_add_class(ctx, cls); 
        }
        g_strlcpy(sl->cur_class, safe, sizeof(sl->cur_class));
    }
}

static GtkWidget *create_popup_label(void) { 
    GtkWidget *l = gtk_label_new(NULL); 
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "sysmon-label"); 
    return l; 
}

/**
 * Dynamically resets classes and configures styling on popup text fields.
 */
static void set_popup_label(GtkWidget *w, const char *text, const char *cls) {
    gtk_label_set_text(GTK_LABEL(w), text); 
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    
    for (int i = 0; ALL_DYN_CLASSES[i]; i++) {
        gtk_style_context_remove_class(ctx, ALL_DYN_CLASSES[i]);
    }
    if (cls) {
        gtk_style_context_add_class(ctx, cls);
    }
}

/**
 * Closes the popup window and safely stops the active UI-refresh timers.
 */
static gboolean close_popup(GtkWidget *widget, GdkEventFocus *event, gpointer data) {
    SysMonPlugin *s = data; 
    if (s->popup) { 
        gtk_widget_hide(s->popup); 
        s->last_hide_time = g_get_monotonic_time(); 
        if (s->popup_timer_id) { 
            g_source_remove(s->popup_timer_id); 
            s->popup_timer_id = 0; 
        } 
    }
    return FALSE;
}

static int dir_filter_hwmon(const struct dirent *e) { return !strncmp(e->d_name, "hwmon", 5); }
static int dir_filter_thermal(const struct dirent *e) { return !strncmp(e->d_name, "thermal_zone", 12); }

/**
 * Safely adds structural separations between logical sensor groupings in the popup.
 */
static void add_separator_if_needed(GtkWidget *box, int *prev, int cur) { 
    if (*prev != -1 && *prev != cur) {
        gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0); 
    }
    *prev = cur; 
}

/**
 * Prepares and registers an active row in the sub-sensor monitor columns.
 */
static void add_popup_sensor_row(SysMonPlugin *sysmon, GtkWidget *col_left, GtkWidget *col_right, 
                                 int *prev_left, int *prev_right, const char *display_name, 
                                 const char *sensor_path, int group, int use_left_column) {
    /* Guard clause to prevent buffer overflow or rendering blank rows */
    if (sysmon->popup_sensor_count >= MAX_POPUP_SENSORS) return;

    GtkWidget *target = use_left_column ? col_left : col_right;
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    int *prev = use_left_column ? prev_left : prev_right; 
    
    add_separator_if_needed(target, prev, group); 
    gtk_widget_set_halign(hbox, GTK_ALIGN_START);
    
    GtkWidget *l_name = create_popup_label();
    GtkWidget *l_val = create_popup_label(); 
    char name_buf[64]; 
    
    snprintf(name_buf, sizeof(name_buf), "%s: ", display_name); 
    set_popup_label(l_name, name_buf, "sysmon-static-c5");
    
    PopupSensorRef *ref = &sysmon->popup_sensors[sysmon->popup_sensor_count]; 
    ref->l_val = l_val; 
    g_strlcpy(ref->path, sensor_path, sizeof(ref->path));
    ref->fd = open(sensor_path, O_RDONLY); 
    ref->cur_class[0] = ref->cur_text[0] = '\0'; 
    sysmon->popup_sensor_count++;
    
    gtk_box_pack_start(GTK_BOX(hbox), l_name, FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(hbox), l_val, FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(target), hbox, FALSE, FALSE, 0);
}

/**
 * Orients the active popup panel window to align with panel position layouts.
 */
static void position_popup(SysMonPlugin *sysmon) {
    GtkWidget *plug = GTK_WIDGET(sysmon->plugin);
    GtkWidget *popup = sysmon->popup; 
    gtk_widget_realize(popup); 
    
    GtkRequisition req; 
    gtk_widget_get_preferred_size(popup, NULL, &req);
    
    gint x = 0, y = 0; 
    GdkWindow *win = gtk_widget_get_window(plug); 
    if (win) {
        gdk_window_get_origin(win, &x, &y);
    }
    
    GtkAllocation alloc; 
    gtk_widget_get_allocation(plug, &alloc); 
    GdkDisplay *display = gtk_widget_get_display(plug);
    GdkMonitor *mon = win ? gdk_display_get_monitor_at_window(display, win) : gdk_display_get_primary_monitor(display);
    
    GdkRectangle geom = {0, 0, 800, 600}; 
    if (mon) {
        gdk_monitor_get_workarea(mon, &geom); 
    }
    gint tx = x, ty = y;
    
    if (xfce_panel_plugin_get_mode(sysmon->plugin) == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL) {
        tx = x + (alloc.width / 2) - (req.width / 2); 
        ty = (y + alloc.height / 2 < geom.y + geom.height / 2) ? y + alloc.height + POPUP_OFFSET : y - req.height - POPUP_OFFSET;
    } else {
        ty = y + (alloc.height / 2) - (req.height / 2); 
        tx = (x + alloc.width / 2 < geom.x + geom.width / 2) ? x + alloc.width + POPUP_OFFSET : x - req.width - POPUP_OFFSET;
    }
    
    gtk_window_move(GTK_WINDOW(popup), CLAMP(tx, geom.x, geom.x + geom.width - req.width), CLAMP(ty, geom.y, geom.y + geom.height - req.height));
}

/**
 * Periodically updates the active sensors within the popup window.
 */
static gboolean update_popup_sensors(gpointer data) {
    SysMonPlugin *s = data; 
    if (!s->popup || s->popup_sensor_count <= 0) { 
        s->popup_timer_id = 0; 
        return G_SOURCE_REMOVE; 
    }
    
    char buf[32];
    for (int i = 0; i < s->popup_sensor_count; i++) {
        PopupSensorRef *ref = &s->popup_sensors[i]; 
        long t = read_sys_fd(ref->fd) / 1000; 
        snprintf(buf, sizeof(buf), "%ld°C", t);
        const char *cls = LVL_CLASSES[get_level(t, 45, 55, 75, 85)];
        
        if (strcmp(ref->cur_text, buf) || strcmp(ref->cur_class, cls)) {
            gtk_label_set_text(GTK_LABEL(ref->l_val), buf); 
            g_strlcpy(ref->cur_text, buf, sizeof(ref->cur_text));
            if (strcmp(ref->cur_class, cls)) {
                GtkStyleContext *ctx = gtk_widget_get_style_context(ref->l_val); 
                if (ref->cur_class[0]) {
                    gtk_style_context_remove_class(ctx, ref->cur_class);
                }
                gtk_style_context_add_class(ctx, cls); 
                g_strlcpy(ref->cur_class, cls, sizeof(ref->cur_class));
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

/**
 * Safely closes active file descriptors opened by popup scans.
 */
static void close_popup_fds(SysMonPlugin *s) {
    for (int i = 0; i < s->popup_sensor_count; i++) { 
        if (s->popup_sensors[i].fd >= 0) { 
            close(s->popup_sensors[i].fd); 
            s->popup_sensors[i].fd = -1; 
        } 
    }
    s->popup_sensor_count = 0;
}

/**
 * Triggers resource teardown of popup parameters on destruction.
 */
static void on_popup_destroy(GtkWidget *widget, gpointer data) {
    SysMonPlugin *s = data; 
    if (s->popup_timer_id) { 
        g_source_remove(s->popup_timer_id); 
        s->popup_timer_id = 0; 
    }
    close_popup_fds(s); 
    s->popup = NULL;
}

/**
 * Generates the full window frame structural components for the sub-sensors.
 */
static void build_popup_structure(SysMonPlugin *sysmon) {
    sysmon->popup = gtk_window_new(GTK_WINDOW_TOPLEVEL); 
    gtk_window_set_decorated(GTK_WINDOW(sysmon->popup), FALSE); 
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(sysmon->popup), TRUE);
    
    GdkScreen *screen = gtk_widget_get_screen(sysmon->popup); 
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen); 
    if (visual) {
        gtk_widget_set_visual(sysmon->popup, visual);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(sysmon->popup), "sysmon-popup-window");
    
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); 
    gtk_style_context_add_class(gtk_widget_get_style_context(container), "sysmon-popup-container");
    GtkWidget *hbox_columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); 
    gtk_box_pack_start(GTK_BOX(container), hbox_columns, TRUE, TRUE, 0);
    
    GtkWidget *col_left  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *sep       = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *col_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    
    gtk_widget_set_valign(col_left, GTK_ALIGN_CENTER); 
    gtk_widget_set_valign(col_right, GTK_ALIGN_CENTER);
    
    gtk_box_pack_start(GTK_BOX(hbox_columns), col_left, TRUE, TRUE, 0); 
    gtk_box_pack_start(GTK_BOX(hbox_columns), sep, FALSE, TRUE, 0); 
    gtk_box_pack_start(GTK_BOX(hbox_columns), col_right, TRUE, TRUE, 0);
    
    int prev_left = -1, prev_right = -1, cnt; 
    struct dirent **namelist = NULL; 
    char path[64], buf[128];
    
    /* Hardware Monitor auto-discovery search */
    if ((cnt = scandir("/sys/class/hwmon", &namelist, dir_filter_hwmon, alphasort)) >= 0) {
        for (int i = 0; i < cnt; i++) {
            if (!namelist[i]) continue;
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", namelist[i]->d_name); 
            read_sys_str(path, buf, sizeof(buf));
            
            if (!strcmp(buf, "acpitz") || !strcmp(buf, "dell_smm")) { 
                free(namelist[i]); 
                continue; 
            }
            
            char dev_name[16] = "";
            if      (!strcmp(buf, "pch_skylake")) g_strlcpy(dev_name, "PCH",  sizeof(dev_name));
            else if (!strcmp(buf, "iwlwifi_1"))   g_strlcpy(dev_name, "WiFi", sizeof(dev_name));
            else if (strcmp(buf, "coretemp") && strcmp(buf, "nvme")) g_strlcpy(dev_name, buf, sizeof(dev_name));
            
            for (int j = 1; j <= 20; j++) {
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_input", namelist[i]->d_name, j); 
                if (access(path, F_OK) != 0) continue;
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_label", namelist[i]->d_name, j); 
                read_sys_str(path, buf, sizeof(buf));
                
                char s_lbl[16] = "";
                if      (!strcmp(buf, "Package id 0")) g_strlcpy(s_lbl, "CPU",    sizeof(s_lbl));
                else if (!strcmp(buf, "Composite"))    g_strlcpy(s_lbl, "NVMe",   sizeof(s_lbl));
                else if (!strcmp(buf, "Sensor 1"))     g_strlcpy(s_lbl, "NAND",   sizeof(s_lbl));
                else if (!strcmp(buf, "Sensor 2"))     g_strlcpy(s_lbl, "ASIC",   sizeof(s_lbl));
                else if (buf[0])                       g_strlcpy(s_lbl, buf,      sizeof(s_lbl));
                
                int is_cpu = (!strcmp(buf, "Package id 0") || !strncmp(buf, "Core ", 5));
                int group = (!strcmp(s_lbl, "NVMe") || !strcmp(s_lbl, "NAND") || !strcmp(s_lbl, "ASIC")) ? 1 : is_cpu ? 2 : (!strcmp(dev_name, "PCH") || !strcmp(dev_name, "WiFi")) ? 3 : 4;
                
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_input", namelist[i]->d_name, j);
                char display[16]; 
                g_strlcpy(display, s_lbl[0] ? s_lbl : dev_name, sizeof(display));
                int left = is_cpu || !strcmp(dev_name, "PCH"); 
                
                add_popup_sensor_row(sysmon, col_left, col_right, &prev_left, &prev_right, display, path, group, left);
            }
            free(namelist[i]);
        }
        free(namelist);
    }
    
    /* ACPI Thermal Zone auto-discovery search */
    if ((cnt = scandir("/sys/class/thermal", &namelist, dir_filter_thermal, alphasort)) >= 0) {
        for (int i = 0; i < cnt; i++) {
            if (!namelist[i]) continue;
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", namelist[i]->d_name); 
            read_sys_str(path, buf, sizeof(buf));
            if (!strcmp(buf, "acpitz") || !strcmp(buf, "pch_skylake") || !strcmp(buf, "iwlwifi_1") || !strcmp(buf, "x86_pkg_temp") || !strncmp(buf, "INT3400", 7)) { 
                free(namelist[i]); 
                continue; 
            }
            
            char lbl[16];
            if      (!strcmp(buf, "SEN1")) g_strlcpy(lbl, "VRM", sizeof(lbl));
            else if (!strcmp(buf, "SEN2")) g_strlcpy(lbl, "AMB", sizeof(lbl));
            else if (!strcmp(buf, "TMEM")) g_strlcpy(lbl, "RAM", sizeof(lbl));
            else if (!strcmp(buf, "B0D4")) g_strlcpy(lbl, "GPU", sizeof(lbl));
            else                           g_strlcpy(lbl, buf,  sizeof(lbl));
            
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", namelist[i]->d_name);
            int group = 4, left = 0;
            if      (!strcmp(lbl, "VRM") || !strcmp(lbl, "RAM") || !strcmp(lbl, "AMB")) group = 5;
            else if (!strcmp(lbl, "GPU")) { group = 3; left = 1; }
            add_popup_sensor_row(sysmon, col_left, col_right, &prev_left, &prev_right, lbl, path, group, left); 
            free(namelist[i]);
        }
        free(namelist);
    }
    
    gtk_container_add(GTK_CONTAINER(sysmon->popup), container); 
    gtk_widget_set_can_focus(sysmon->popup, TRUE);
    g_signal_connect(sysmon->popup, "focus-out-event", G_CALLBACK(close_popup), sysmon);
    g_signal_connect(sysmon->popup, "destroy",         G_CALLBACK(on_popup_destroy), sysmon);
}

static void show_popup(SysMonPlugin *s) {
    if (!s->popup) { build_popup_structure(s); }
    if (!s->popup) return;
    if (gtk_widget_get_visible(s->popup)) { 
        gtk_widget_hide(s->popup); 
        s->last_hide_time = g_get_monotonic_time(); 
        if (s->popup_timer_id) { 
            g_source_remove(s->popup_timer_id); 
            s->popup_timer_id = 0; 
        } 
        return; 
    }
    if (g_get_monotonic_time() - s->last_hide_time < 100000) return;
    update_popup_sensors(s); 
    gtk_widget_show_all(s->popup); 
    position_popup(s); 
    s->popup_timer_id = g_timeout_add(UPDATE_INTERVAL_MS, update_popup_sensors, s);
}

static gboolean on_click(GtkWidget *w, GdkEventButton *e, gpointer d) { 
    show_popup((SysMonPlugin*)d); 
    return TRUE; 
}

/**
 * Primary callback responsible for reading system state files, updating widgets,
 * and executing thermal control loops.
 */
static gboolean update_display(gpointer user_data) {
    SysMonPlugin *s = user_data; 
    char buf[128];
    unsigned long long u = 0, ni = 0, sy = 0, id = 0, wa = 0, irq = 0, sirq = 0, st = 0;
    
    /* Read CPU utilization counters */
    if (s->fds[FD_STAT] >= 0) {
        char stat_buf[512]; 
        ssize_t n = pread(s->fds[FD_STAT], stat_buf, sizeof(stat_buf) - 1, 0);
        if (n > 0) {
            stat_buf[n] = '\0'; 
            char *ptr = stat_buf;
            if (!strncmp(ptr, "cpu", 3)) {
                ptr += 3;
                u = quick_parse_ull(&ptr);  ni = quick_parse_ull(&ptr);   sy = quick_parse_ull(&ptr);
                id = quick_parse_ull(&ptr);  wa = quick_parse_ull(&ptr);   irq = quick_parse_ull(&ptr);
                sirq = quick_parse_ull(&ptr); st = quick_parse_ull(&ptr);
            }
        }
    }
    
    /* Calculate utilization percentages based on differences between ticks */
    unsigned long long total = u + ni + sy + id + wa + irq + sirq + st, idle = id + wa; 
    int usage = 1;
    if (total > s->pt && idle >= s->pi) { 
        unsigned long long dt = total - s->pt, di = idle - s->pi; 
        if (dt >= di) usage = (int)(100ULL * (dt - di) / dt); 
    }
    
    usage = CLAMP(usage, 1, 99); 
    s->pt = total; 
    s->pi = idle; 
    snprintf(buf, sizeof(buf), " %02d%% ", usage); 
    set_label(&s->l_cpu_u, buf, LVL_CLASSES[get_level(usage, 10, 40, 70, 90)]);
    
    /* CPU Core Frequencies */
    long cpu_f = read_sys_fd(s->fds[FD_CPUF]); 
    snprintf(buf, sizeof(buf), "%ld.%02ld GHz ", cpu_f / 1000000, (cpu_f % 1000000) / 10000); 
    set_label(&s->l_cpu_f, buf, LVL_CLASSES[get_level(cpu_f, 1200000, 2000000, 2800000, 3400000)]);
    
    /* CPU Temperatures */
    long temp = read_sys_fd(s->fds[FD_TEMP]) / 1000; 
    snprintf(buf, sizeof(buf), "%ld°C ", temp); 
    set_label(&s->l_temp, buf, LVL_CLASSES[get_level(temp, 45, 55, 75, 85)]); 
    set_label(&s->l_sep, "❘ ", "sysmon-sep");
    
    /* GPU Clock Speeds */
    long gpu_f = read_sys_fd(s->fds[FD_GPUF]); 
    snprintf(buf, sizeof(buf), "%ld.%02ld GHz ", gpu_f / 1000, (gpu_f % 1000) / 10); 
    set_label(&s->l_gpu_f, buf, LVL_CLASSES[get_level(gpu_f, 450, 650, 850, 1000)]);
    
    /* Fan RPM Speeds */
    long rpm = read_sys_fd(s->fds[FD_FAN]); 
    if (rpm > 0) { 
        snprintf(buf, sizeof(buf), "%ld rpm", rpm); 
        set_label(&s->l_fan, buf, "sysmon-lvl-4"); 
    } else {
        set_label(&s->l_fan, " _____ rpm", "sysmon-lvl-0");
    }
    
    /* Dynamic Thermal Throttling Logic */
    int new_throttle = s->throttle_lvl;
    if (s->throttle_lvl == -1) { 
        new_throttle = (temp >= 85) ? 3 : (temp >= 80) ? 2 : (temp >= 75) ? 1 : 0; 
        s->cool_ticks = 0; 
    } else {
        if      (temp >= 85) { new_throttle = 3; s->cool_ticks = 0; }
        else if (temp >= 80 && s->throttle_lvl < 2) { new_throttle = 2; s->cool_ticks = 0; }
        else if (temp >= 75 && s->throttle_lvl < 1) { new_throttle = 1; s->cool_ticks = 0; }
        else {
            int target = s->throttle_lvl;
            int drop = (s->throttle_lvl == 3 && temp <= 78) ? (target = 2, 1) : 
                      (s->throttle_lvl == 2 && temp <= 72) ? (target = 1, 1) : 
                      (s->throttle_lvl == 1 && temp <= 65) ? (target = 0, 1) : 0;
            if (drop && ++s->cool_ticks >= COOLDOWN_TICKS_REQ) { 
                new_throttle = target; 
                s->cool_ticks = 0; 
            } else if (!drop) {
                s->cool_ticks = 0;
            }
        }
    }
    
    /* Change performance modes based on threshold state transitions */
    if (new_throttle != s->throttle_lvl) {
        s->throttle_lvl = new_throttle; 
        const char *msg;
        if      (new_throttle == 3) { write_sys(CPU_MAX_PCT,"50"); write_sys(GPU_MAX,"300"); write_sys(GPU_BOOST,"300"); msg = "Level 3 Throttling"; }
        else if (new_throttle == 2) { write_sys(CPU_MAX_PCT,"70"); write_sys(GPU_MAX,"600"); write_sys(GPU_BOOST,"600"); msg = "Level 2 Throttling"; }
        else if (new_throttle == 1) { write_sys(CPU_MAX_PCT,"85"); write_sys(GPU_MAX,"900"); write_sys(GPU_BOOST,"900"); msg = "Level 1 Throttling"; }
        else                     { write_sys(CPU_MAX_PCT,"100"); write_sys(GPU_MAX,"1100"); write_sys(GPU_BOOST,"1100"); msg = "System Unconstrained"; }
        if (strcmp(msg, s->f_notify)) { 
            g_strlcpy(s->f_notify, msg, sizeof(s->f_notify)); 
            send_state_notify(&s->n_throttle, msg, new_throttle == 3, s->notify_initialized); 
        }
    }
    
    /* Memory Statistics Parsing */
    long mem_total = 0, mem_avail = 0;
    if (s->fds[FD_MEM] >= 0) {
        char mem_buf[1024]; 
        ssize_t n = pread(s->fds[FD_MEM], mem_buf, sizeof(mem_buf) - 1, 0);
        if (n > 0) {
            mem_buf[n] = '\0'; 
            char *mt = strstr(mem_buf, "MemTotal:");
            if (mt) { 
                char *p = mt + 9; 
                mem_total = quick_parse_ull(&p); 
                char *ma = strstr(p, "MemAvailable:"); 
                if (ma) { 
                    p = ma + 13; 
                    mem_avail = quick_parse_ull(&p); 
                } 
            }
        }
    }
    int mem_u = 0; 
    if (mem_total > 0) { 
        long used = mem_total - mem_avail; 
        mem_u = (int)((used > 0 ? used : 0) / 104857); 
    }
    snprintf(buf, sizeof(buf), " %d.%d GB ", mem_u / 10, mem_u % 10); 
    set_label(&s->l_mem, buf, LVL_CLASSES[get_level(mem_u, 20, 35, 50, 65)]);
    
    /* Battery Capacity and Power Charging States */
    int cap = (int)read_sys_fd(s->fds[FD_BATC]); 
    char bst_char = '\0'; 
    if (s->fds[FD_BATS] >= 0) pread(s->fds[FD_BATS], &bst_char, 1, 0);
    const char *bat_class = "sysmon-lvl-0";
    if (bst_char != 'D') 
        bat_class = (bst_char == 'C') ? "sysmon-lvl-1-charging" : (bst_char == 'N') ? "sysmon-ncharge" : "sysmon-lvl-0";
    else 
        bat_class = (cap <= 25) ? "sysmon-lvl-4" : (cap <= 35) ? "sysmon-lvl-3" : "sysmon-lvl-0";
    snprintf(buf, sizeof(buf), "%d%% ", cap); 
    set_label(&s->l_bat, buf, bat_class);
    
    if (bst_char == 'D') {
        if (s->f_ac) { 
            send_state_notify(&s->n_power, "Running on Battery", 0, s->notify_initialized); 
            s->f_ac = 0; 
        }
        if (cap <= 15 && !s->f_b15) { 
            send_state_notify(&s->n_power, "Connect Charger!", 1, s->notify_initialized); 
            s->f_b15 = 1; 
        }
    } else if (!s->f_ac) { 
        send_state_notify(&s->n_power, "AC Connected", 0, s->notify_initialized); 
        s->f_ac = 1; 
        s->f_b15 = 0; 
    }
    
    return G_SOURCE_CONTINUE;
}

/**
 * Standard destructor callback: cleans up glib tasks, closes files, 
 * and releases allocated tracking contexts.
 */
static void plugin_free(XfcePanelPlugin *p, SysMonPlugin *s) {
    if (s->main_timer_id) { 
        g_source_remove(s->main_timer_id); 
        s->main_timer_id = 0;
    }
    if (s->popup_timer_id) { 
        g_source_remove(s->popup_timer_id); 
        s->popup_timer_id = 0;
    }
    if (s->popup) { 
        g_signal_handlers_disconnect_by_func(s->popup, G_CALLBACK(on_popup_destroy), s); 
        gtk_widget_destroy(s->popup); 
    }
    close_popup_fds(s);
    if (s->n_throttle) { 
        g_object_unref(s->n_throttle); 
        s->n_throttle = NULL;
    }
    if (s->n_power) { 
        g_object_unref(s->n_power); 
        s->n_power = NULL;
    }
    if (--notify_ref_count <= 0) { 
        if (s->notify_initialized) {
            notify_uninit();
        }
        notify_ref_count = 0; 
    }
    for (int i = 0; i < FD_COUNT; i++) { 
        if (s->fds[i] >= 0) {
            close(s->fds[i]); 
            s->fds[i] = -1;
        }
    }
    g_free(s);
}

/**
 * Standard initialization entry point: prepares contexts, loads styles, 
 * binds events, and maps core kernel files.
 */
static void plugin_construct(XfcePanelPlugin *plugin) {
    SysMonPlugin *s = g_new0(SysMonPlugin, 1); 
    s->plugin = plugin; 
    s->throttle_lvl = -1; 
    s->cool_ticks = 0;
    s->notify_initialized = FALSE;
    
    /* Defensive check to ensure desktop notifications initialization succeeded */
    if (notify_ref_count++ == 0) {
        s->notify_initialized = notify_init("SysMonitor");
    } else {
        s->notify_initialized = TRUE; 
    }
    
    setup_css();
    
    for (int i = 0; i < FD_COUNT; i++) s->fds[i] = -1;
    for (int i = 0; i < MAX_POPUP_SENSORS; i++) { 
        s->popup_sensors[i].fd = -1; 
        s->popup_sensors[i].cur_class[0] = s->popup_sensors[i].cur_text[0] = '\0'; 
    }
    
    /* Open persistent files */
    s->fds[FD_STAT] = open("/proc/stat", O_RDONLY); 
    s->fds[FD_MEM]  = open("/proc/meminfo", O_RDONLY);
    s->fds[FD_CPUF] = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY); 
    s->fds[FD_GPUF] = open(GPU_ACTUAL, O_RDONLY);
    s->fds[FD_BATC] = open("/sys/class/power_supply/BAT0/capacity", O_RDONLY); 
    s->fds[FD_BATS] = open("/sys/class/power_supply/BAT0/status", O_RDONLY);
    
    DIR *dr = opendir("/sys/class/hwmon");
    if (dr) {
        struct dirent *de;
        while ((de = readdir(dr)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char np[64], name[64]; 
            snprintf(np, sizeof(np), "/sys/class/hwmon/%s/name", de->d_name); 
            read_sys_str(np, name, sizeof(name));
            
            /* Defensive checks to prevent file descriptor leaks on system scan loop */
            if (!strcmp(name, "coretemp") && s->fds[FD_TEMP] < 0) {
                char temp_path[64]; 
                snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/%s/temp1_input", de->d_name); 
                s->fds[FD_TEMP] = open(temp_path, O_RDONLY);
            }
            
            if (s->fds[FD_FAN] < 0) {
                char fan_test[64]; 
                snprintf(fan_test, sizeof(fan_test), "/sys/class/hwmon/%s/fan1_input", de->d_name);
                if (!access(fan_test, F_OK)) {
                    s->fds[FD_FAN] = open(fan_test, O_RDONLY);
                }
            }
        }
        closedir(dr);
    }
    
    char bst_char = '\0'; 
    if (s->fds[FD_BATS] >= 0) pread(s->fds[FD_BATS], &bst_char, 1, 0); 
    s->f_ac = (bst_char != 'D');
    
    GtkWidget *ebox = gtk_event_box_new();
    GtkWidget *box  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); 
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
    
    sl_init(&s->l_cpu_u); sl_init(&s->l_cpu_f); sl_init(&s->l_temp); sl_init(&s->l_sep); 
    sl_init(&s->l_gpu_f); sl_init(&s->l_fan); sl_init(&s->l_mem); sl_init(&s->l_bat);
    
    gtk_box_pack_start(GTK_BOX(box), s->l_cpu_u.widget, FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(box), s->l_cpu_f.widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->l_temp.widget,  FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(box), s->l_sep.widget,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->l_gpu_f.widget, FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(box), s->l_fan.widget,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->l_mem.widget,   FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(box), s->l_bat.widget,   FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(ebox), box); 
    gtk_container_add(GTK_CONTAINER(plugin), ebox);
    
    /* Removed build_popup_structure here to enable optimal lazy loading */
    
    g_signal_connect(ebox, "button-press-event", G_CALLBACK(on_click), s); 
    g_signal_connect(plugin, "free-data", G_CALLBACK(plugin_free), s);
    
    update_display(s); 
    s->main_timer_id = g_timeout_add(UPDATE_INTERVAL_MS, update_display, s); 
    gtk_widget_show_all(GTK_WIDGET(plugin));
}

XFCE_PANEL_PLUGIN_REGISTER(plugin_construct);