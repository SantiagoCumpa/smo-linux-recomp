#!/usr/bin/env python3
"""The game folder's launcher: a GTK4 + libadwaita window with the options of config/graphics.ini and smo.env, and a
Play button that starts the game (./smo-run).

    python3 launcher/smo_launcher.py [--folder <game folder>]        the window
    python3 launcher/smo_launcher.py --folder <f> --show              print the values it would show
    python3 launcher/smo_launcher.py --folder <f> --set key=value ... change graphics.ini / smo.env without a window

The game folder is the one tools/make_game_folder.py makes (default: the folder above this file). graphics.ini keeps its
comments and every key the launcher does not change: only the 'key = value' lines it changes are rewritten, a missing
key is added at the end. smo.env keeps its other lines (export NAME=value). Keys of smo.env: SMO_LANGUAGE (the game's
language, nnmisc.c), SMO_LAUNCHER (0: ./smo starts the game without this window).
"""
import argparse
import os
import re
import subprocess
import sys

# (value written, label shown); the first entry is the default when the file does not say
RESOLUTIONS = [('900p', '900p · 1600×900 (the game\'s own)'), ('720p', '720p · 1280×720'), ('1080p', '1080p · 1920×1080'),
               ('1440p', '1440p · 2560×1440 (2K)'), ('2160p', '2160p · 3840×2160 (4K)')]
WINDOW_MODES = [('borderless_fullscreen', 'Full screen'), ('fullscreen', 'Full screen, always on top'),
                ('borderless_window', 'Borderless window'), ('windowed', 'Window')]
WINDOW_SIZES = [('1280x720', '1280×720'), ('1600x900', '1600×900'), ('1920x1080', '1920×1080'), ('2560x1440', '2560×1440'),
                ('3840x2160', '3840×2160')]
VSYNC = [('clock', 'Game clock (lowest latency)'), ('on', 'On'), ('off', 'Off'), ('mailbox', 'Mailbox')]
GPUS = [('auto', 'Automatic (discrete first)'), ('NVIDIA', 'NVIDIA'), ('AMD', 'AMD'), ('Intel', 'Intel')]
ANISO = [('game', 'The game\'s own'), ('2', '2×'), ('4', '4×'), ('8', '8×'), ('16', '16×')]
SHADER_WAIT = [('hold', 'Wait (correct picture, some stutter)'), ('skip', 'Skip (no stutter, effects missing for a moment)')]
UNFOCUSED = [('run', 'Keep playing'), ('pause', 'Pause, like the HOME menu')]
FPS_OVERLAY = [('off', 'Off'), ('top_left', 'Top left'), ('top_right', 'Top right'), ('bottom_left', 'Bottom left'),
               ('bottom_right', 'Bottom right')]
LANGUAGES = [('', 'System language'), ('es', 'Español (España)'), ('es-419', 'Español (Latinoamérica)'),
             ('en-US', 'English (US)'), ('en-GB', 'English (UK)'), ('fr', 'Français'), ('fr-CA', 'Français (Canada)'),
             ('de', 'Deutsch'), ('it', 'Italiano'), ('nl', 'Nederlands'), ('ru', 'Русский'), ('ja', '日本語'),
             ('ko', '한국어'), ('zh-Hans', '简体中文'), ('zh-Hant', '繁體中文')]
GRAPHICS_KEYS = ('resolution', 'window', 'width', 'height', 'vsync', 'gpu', 'anisotropic_filtering', 'profile',
                 'shader_wait', 'fps_overlay', 'frame_time_overlay', 'unfocused', 'screen_saver')
ENV_KEYS = ('SMO_LANGUAGE', 'SMO_LAUNCHER')
SCALE_TO_RES = {0.8: '720p', 1.0: '900p', 1.2: '1080p', 1.6: '1440p', 2.4: '2160p'}


class Ini:
    """graphics.ini as lines: values are read and changed in place, everything else is kept."""
    LINE = re.compile(r'^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*=\s*)(.*?)(\s*)$')

    def __init__(self, path):
        self.path = path
        self.lines = open(path, encoding='utf-8').read().split('\n') if os.path.exists(path) else []

    def _find(self, key):
        for i, line in enumerate(self.lines):
            m = self.LINE.match(line)
            if m and m.group(2).lower() == key.lower():
                return i, m
        return None, None

    def get(self, key, default=None):
        _, m = self._find(key)
        return m.group(4) if m else default

    def set(self, key, value):
        i, m = self._find(key)
        if m:
            if m.group(4) != value:
                self.lines[i] = m.group(1) + m.group(2) + m.group(3) + value + m.group(5)
            return
        if not self.lines:
            self.lines = ['; graphics.ini, written by the launcher. The game documents every key when the file does not exist:',
                          '; rename this file once to see them all.', '']
        while self.lines and self.lines[-1] == '':
            self.lines.pop()
        self.lines += [key + ' = ' + value, '']

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + '.new'
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.lines))
        os.replace(tmp, self.path)


class Env:
    """smo.env: 'export NAME=value' lines read by ./smo and ./smo-run; other lines are kept."""
    LINE = re.compile(r'^\s*(?:export\s+)?([A-Z_][A-Z0-9_]*)=(.*)$')

    def __init__(self, path):
        self.path = path
        self.lines = open(path, encoding='utf-8').read().split('\n') if os.path.exists(path) else []

    def get(self, key, default=''):
        for line in self.lines:
            m = self.LINE.match(line)
            if m and m.group(1) == key:
                return m.group(2).strip().strip('"\'')
        return default

    def set(self, key, value):
        """value '' removes the key."""
        out, done = [], False
        for line in self.lines:
            m = self.LINE.match(line)
            if m and m.group(1) == key:
                if value != '' and not done:
                    out.append('export %s=%s' % (key, value))
                done = True
                continue
            out.append(line)
        if not done and value != '':
            if not out:
                out = ['# settings of this game folder, read by ./smo and ./smo-run']
            while out and out[-1] == '':
                out.pop()
            out.append('export %s=%s' % (key, value))
        self.lines = out

    def save(self):
        tmp = self.path + '.new'
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.lines).rstrip('\n') + '\n')
        os.replace(tmp, self.path)


class Settings:
    """What the window shows and changes: graphics.ini + smo.env of one game folder."""

    def __init__(self, folder):
        self.folder = folder
        self.ini = Ini(os.path.join(folder, 'config', 'graphics.ini'))
        self.env = Env(os.path.join(folder, 'smo.env'))

    def get(self, key):
        if key in ENV_KEYS:
            return self.env.get(key, '1' if key == 'SMO_LAUNCHER' else '')
        if key == 'resolution':
            res = self.ini.get('resolution')
            if res:
                return res.lower().replace('4k', '2160p').replace('2k', '1440p')
            try:
                return SCALE_TO_RES.get(round(float(self.ini.get('resolution_scale', '1.0')), 2), '900p')
            except ValueError:
                return '900p'
        defaults = {'window': 'borderless_fullscreen', 'width': '1280', 'height': '720', 'vsync': 'clock', 'gpu': 'auto',
                    'anisotropic_filtering': 'game', 'profile': 'custom', 'shader_wait': 'hold',
                    'fps_overlay': 'off', 'frame_time_overlay': 'off', 'unfocused': 'run', 'screen_saver': 'off_while_playing'}
        return self.ini.get(key, defaults.get(key, ''))

    def set(self, key, value):
        if key in ENV_KEYS:
            self.env.set(key, value)
        elif key in GRAPHICS_KEYS:
            self.ini.set(key, value)
        else:
            raise KeyError(key)

    def save(self):
        self.ini.save()
        self.env.save()


def default_folder():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def start_game(folder, args=()):
    """Starts ./smo-run on its own (the launcher can close at once)."""
    run = os.path.join(folder, 'smo-run')
    return subprocess.Popen([run] + list(args), cwd=folder, start_new_session=True,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def open_path(path):
    subprocess.Popen(['xdg-open', path], start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------- the window
def run_window(folder):
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Adw', '1')
    from gi.repository import Adw, Gio, Gtk

    s = Settings(folder)

    class Window(Adw.ApplicationWindow):
        def __init__(self, app):
            super().__init__(application=app, title='Super Mario Odyssey')
            self.set_default_size(560, 720)
            self.rows = {}
            toolbar = Adw.ToolbarView()
            header = Adw.HeaderBar()
            header.set_title_widget(Adw.WindowTitle(title='Super Mario Odyssey', subtitle='smo-recomp · Linux'))
            toolbar.add_top_bar(header)

            page = Adw.PreferencesPage()
            screen = Adw.PreferencesGroup(title='Display')
            self.res = self.combo(screen, 'resolution', 'Internal resolution', RESOLUTIONS,
                                  'The resolution the game draws at; above 900p: more detail and more GPU load')
            self.mode = self.combo(screen, 'window', 'Mode', WINDOW_MODES, 'F11 or a double-click switch while playing')
            size = '%sx%s' % (s.get('width'), s.get('height'))
            self.size = self.combo_value(screen, 'Window size', WINDOW_SIZES, size, 'Window modes only')
            self.combo(screen, 'vsync', 'Vertical sync', VSYNC)
            page.add(screen)

            gfx = Adw.PreferencesGroup(title='Graphics')
            self.combo(gfx, 'gpu', 'GPU', GPUS, 'On a laptop with two GPUs, Automatic uses the discrete one')
            self.aniso = self.combo(gfx, 'anisotropic_filtering', 'Anisotropic filtering', ANISO)
            self.low = self.switch(gfx, 'profile', 'Weak GPU profile', ('low', 'custom'),
                                   'Draws at half resolution; resolution and filtering are ignored')
            self.combo(gfx, 'shader_wait', 'Shaders not ready yet', SHADER_WAIT)
            page.add(gfx)

            perf = Adw.PreferencesGroup(title='Performance')
            self.combo(perf, 'fps_overlay', 'FPS counter', FPS_OVERLAY, 'F10 shows or hides it while playing')
            self.switch(perf, 'frame_time_overlay', 'Frame time in the window title', ('on', 'off'))
            page.add(perf)

            system = Adw.PreferencesGroup(title='System')
            self.combo(system, 'unfocused', 'When another window is active', UNFOCUSED)
            self.switch(system, 'screen_saver', 'Keep the screen on', ('off_while_playing', 'allow'),
                        'While the game window is active; a gamepad does not count as activity for the desktop')
            self.combo(system, 'SMO_LANGUAGE', 'Game language', LANGUAGES)
            self.switch(system, 'SMO_LAUNCHER', 'Show this launcher at start', ('1', '0'),
                        'When off, ./smo starts the game directly (./smo --launcher shows it again)')
            page.add(system)

            files = Adw.PreferencesGroup(title='Files')
            for title, rel in (('Saves', os.path.join('userdata', 'save')), ('Screenshots (F12)', os.path.join('userdata', 'album')),
                               ('Last log', os.path.join('logs', 'smo.log')), ('Settings', 'config')):
                row = Adw.ActionRow(title=title, subtitle=rel, activatable=True)
                row.add_suffix(Gtk.Image.new_from_icon_name('document-open-symbolic'))
                row.connect('activated', lambda _r, p=os.path.join(folder, rel): open_path(p if os.path.exists(p) else os.path.dirname(p)))
                files.add(row)
            page.add(files)
            toolbar.set_content(page)

            bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12, margin_top=12, margin_bottom=12,
                          margin_start=12, margin_end=12)
            self.status = Gtk.Label(xalign=0, hexpand=True, wrap=True)
            self.status.add_css_class('dim-label')
            play = Gtk.Button(label='Play')
            play.add_css_class('suggested-action')
            play.add_css_class('pill')
            play.connect('clicked', self.on_play)
            bar.append(self.status)
            bar.append(play)
            toolbar.add_bottom_bar(bar)
            self.set_content(toolbar)
            self.set_default_widget(play)
            self.refresh()

        def combo(self, group, key, title, options, subtitle=None):
            return self.combo_value(group, title, options, s.get(key), subtitle, key)

        def combo_value(self, group, title, options, current, subtitle=None, key=None):
            options = list(options)
            if current not in [v for v, _ in options]:
                options.append((current, current))                 # a value written by hand is kept and shown
            row = Adw.ComboRow(title=title, model=Gtk.StringList.new([label for _, label in options]))
            if subtitle:
                row.set_subtitle(subtitle)
            row.set_selected([v for v, _ in options].index(current))
            row.values = [v for v, _ in options]
            row.connect('notify::selected', lambda *_: self.refresh())
            group.add(row)
            if key:
                self.rows[key] = row
            return row

        def switch(self, group, key, title, values, subtitle=None):
            row = Adw.SwitchRow(title=title)
            if subtitle:
                row.set_subtitle(subtitle)
            row.set_active(s.get(key) == values[0])
            row.values = values
            row.connect('notify::active', lambda *_: self.refresh())
            group.add(row)
            self.rows[key] = row
            return row

        @staticmethod
        def value(row):
            if isinstance(row, Adw.SwitchRow):
                return row.values[0] if row.get_active() else row.values[1]
            return row.values[row.get_selected()]

        def refresh(self):
            low = self.low.get_active()
            self.res.set_sensitive(not low)
            self.aniso.set_sensitive(not low)
            self.size.set_sensitive(self.value(self.mode) in ('borderless_window', 'windowed'))
            res = self.value(self.res)
            self.status.set_label('Weak GPU profile: the game draws at 800×450.' if low else
                                  'High GPU load: lower the resolution if the game does not reach 60 FPS.' if res in ('1440p', '2160p') else '')

        def save(self):
            for key, row in self.rows.items():
                s.set(key, self.value(row))
            w, h = self.value(self.size).split('x')
            s.set('width', w)
            s.set('height', h)
            s.save()

        def on_play(self, _button):
            try:
                self.save()
                start_game(folder)
            except OSError as e:
                self.status.set_label('Could not start the game: %s' % e)
                return
            self.get_application().quit()

    app = Adw.Application(application_id='io.github.smo_recomp.Launcher', flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect('activate', lambda a: Window(a).present())
    return app.run([sys.argv[0]])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--folder', default=default_folder(), help='the game folder (default: the folder above launcher/)')
    ap.add_argument('--show', action='store_true', help='print the values and exit')
    ap.add_argument('--set', nargs='+', metavar='KEY=VALUE', help='change values without a window and exit')
    a = ap.parse_args()
    folder = os.path.abspath(a.folder)
    if a.show or a.set:
        s = Settings(folder)
        for kv in a.set or []:
            k, v = kv.split('=', 1)
            s.set(k, v)
        if a.set:
            s.save()
        for k in GRAPHICS_KEYS + ENV_KEYS:
            print('%s = %s' % (k, s.get(k)))
        return 0
    return run_window(folder)


if __name__ == '__main__':
    sys.exit(main())
