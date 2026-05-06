# `sprintf` kfun

Native C implementation of the `sprintf` kfun for the Hexagon mudlib.

## Loading

Built as `sprintf.1.5` next to the other extension modules. Add to the
DGD config:

```
modules = ([ "/path/to/sprintf.1.5" : "" ]);
```

## Contract

```
string sprintf(string format, mixed args...);
```

Conversion characters: `%s %S %d %i %o %x %X %b %c %a %A %y %f %e %E
%g %G %h %H %n %p %%`.

Flags: `- _ | + ' ' 0 # \` & ~ < > =`.

Width: integer or `*` (next arg).
Precision: `.` integer or `.*` (next arg).

Width and centering are computed in **visible columns** — ANSI CSI
escape sequences and UTF-8 multibyte characters do not inflate the
field.

## Out of scope

- `%r %R` (encrypt) and `%q %Q` (call_other) — not used in Hexagon.
- `@*` time conversions — not implemented.
- `%O` — recognised but raises a runtime error. See "Why `%O` is not
  implemented" below.

## Limitations

- Precision-based truncation (`%.Ns` / `%_.Ns`) is byte-based. UTF-8
  / ANSI text truncated by precision may produce malformed output.
- East Asian fullwidth characters and combining marks count as one
  column each.

## Why `%O` is not implemented

`%O obj` should print `object_name(obj)`. Implementing it inside an
extension kfun requires turning the `LPC_value` we receive as argument
back into an `Object*` so we can call `lpc_object_name(f, obj, buf)`.

The public DGD extension API (`dgd-extensions/src/lpc_ext.h`) exposes
the symmetric `*_getval`/`*_putval` pair for every value type **except
objects**:

| type     | `*_getval`     | `*_putval`     |
|----------|:--------------:|:--------------:|
| int      | yes            | yes            |
| float    | yes            | yes            |
| string   | yes            | yes            |
| **object** | **missing**  | yes            |
| array    | yes            | yes            |
| mapping  | yes            | yes            |

This is a real asymmetry in the API: DGD's own internal kfuns reach the
underlying `Object*` via the private `OBJR(val->oindex)` macro, but
that's not exposed to extensions. Without `lpc_object_getval` there is
no way to resolve an object reference passed in as an `LPC_value`.

The current branch in the kfun therefore raises a clear runtime error
instead of silently returning a placeholder. Callers that want to
print an object name should use:

```lpc
sprintf("%s", object_name(ob))   // instead of "%O", ob
```

A grep of the live Hexagon mudlib at the time this kfun was written
found a single use of `%O` in a disabled test, so the practical cost
of this limitation is essentially zero. The `%s + object_name`
workaround is identical in output and behaviour.

### Path forward (if `%O` ever becomes worth supporting)

Three coordinated changes, all in our forks:

1. `dgd/src/ext.cpp` — add
   ```c
   static Object *ext_object_getval(Value *val) { return OBJR(val->oindex); }
   ```
   register it in `ext_object[6]`, bump `sizes[7]` from 6 to 7, and bump
   `EXTENSION_MINOR`.
2. `dgd-extensions/src/lpc_ext.h` — add the matching
   `LPC_object (*lpc_object_getval)(LPC_value);` declaration and bump
   `LPC_EXT_VERSION_MINOR`.
3. `dgd-extensions/src/lpc_ext.c` — wire the new function pointer into
   `ext_cb(ftabs[7], ...)`, growing the count from 6 to 7.

After that, the `%O` branch in `sprintf.c` can be replaced with the
straightforward
```c
char buf[1024];
const char *name = lpc_object_name(f, lpc_object_getval(v), buf);
align(out, name, strlen(name), width, precision, flags, *padding, *padlen, 0);
```

Eventually it would be worth proposing this upstream to
`dworkin/lpc-ext`; until then it would live as a Maldorne-specific
patch in the fork.
