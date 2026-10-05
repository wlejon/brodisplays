---
name: Unsupported platform or feature
about: brodisplays reports something unsupported (a compositor, a desktop's night light, a display property) that the platform can do
labels: enhancement
---

**What brodisplays reported** (the `Result` error or the capability it says is
missing; it names the reason by design):

```
```

**Where:** OS, session type, desktop or compositor and version.

**How the platform does it:** the protocol, D-Bus interface, OS API or tool
that already does this here (for example `wlr-randr`, `kscreen-doctor`,
`gsettings`, a Windows or CoreGraphics API), so the backend can follow it.

**What you are building:** what you need it for. A real application is the
most useful answer; it is what decides which gaps get closed first.
