import os

# Keep Kivy from consuming pytest's argv when the dispatcher layer is imported headless.
os.environ.setdefault("KIVY_NO_ARGS", "1")

# Some UI modules (e.g. elsbar) load KV that pulls in widgets which, in turn, import
# kivy.core.window — importing that module *creates* a Window. On a headless CI runner with
# no display that hard-crashes the interpreter. Force SDL's dummy video/audio drivers and a
# mock GL backend so any Window created during collection is off-screen and can't crash.
os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
os.environ.setdefault("KIVY_GL_BACKEND", "mock")

# Pin the window provider to sdl2. Without this, Kivy probes providers in order and the
# **x11** provider wins wherever libX11 is installed — it logs
# "Provider: x11(['window_sdl2'] ignored)", talks to Xlib directly (so SDL_VIDEODRIVER=dummy
# above cannot help it), and calls exit(102) with no traceback when there is no DISPLAY.
# That is a silent, output-free death mid-collection, and it only reproduces on hosts that
# have the X libraries but no X server — i.e. a CI runner, not a bare container or a
# developer's desktop. Forcing sdl2 keeps the dummy video driver in charge.
os.environ.setdefault("KIVY_WINDOW", "sdl2")
