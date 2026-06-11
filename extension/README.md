# extension/

The GNOME Shell extension (GJS + Clutter + GStreamer). A separate process
that consumes the renderer's PipeWire stream and draws it as a Clutter
actor in the background layer, beneath the desktop icons.

It links no renderer code; it talks to the renderer only over PipeWire.
**Licence: MIT.** See [ADR-0002](../docs/_adr/ADR-0002-licensing.md).

Test only in a nested shell, never on the live desktop:

```sh
dbus-run-session -- gnome-shell --nested --wayland
```

Empty until Session 3 builds the prototype actor.
