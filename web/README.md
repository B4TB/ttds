# TTDS API

Shapes, colors, and so forth.

## General API Structure

Most interactions with the server begin with a request like this:

```
POST /pane/<name>/create?color=%23XXXXXX
```

Here, you specify two things. First, the pane needs a unique name, which should
ideally be something human-readable, but need not be. Then, you specify the
color. (Note that `%23` is the URL-escaped version of the `#` glyph, so what
you're really doing here is specifying for a hex color preceded by a `#`.) E.g.,

```
POST /pane/example/create?color=%23000000
```

creates a pane named "example" with a solid black background color. On success,
this will return a 200 and give you a UUID. Subsequent requests to the pane must
include this UUID in the HTTP headers so as to prevent accidental requests
overwriting a pane it shouldn't. So, for instance, to draw a rectangle, see for
example

```
POST /pane/example/rect?color=%23000000&x=0&y=0&h=100&w=100
Auth: db0b4cf0-e0a9-49c5-918a-52c13acbcbd7
```

Be sure to provide all of `x`, `y`, `w`, `h`, and a `color` along with an `Auth`
header supplying that UUID you've got. See [below](#reference) for a list of all
allowed methods. When done with a pane, it can be deleted with the same `Auth`
header attached to a `DELETE` request:

```
DELETE /pane/example/rect
Auth: db0b4cf0-e0a9-49c5-918a-52c13acbcbd7
```

## Reference

The following methods are allowed:

| Method | Path                     | Query Parameters                            | Auth Required? | Description                                                            |
|--------|--------------------------|---------------------------------------------|----------------|------------------------------------------------------------------------|
| POST   | /pane/\<name\>           | `color`                                     | No.            | Create a pane with the given color.                                    |
| POST   | /pane/\<name\>/rect      | `color`, `x`, `y`, `w`, `h`                 | Yes.           | Draw a rectangle on a pane.                                            |
| POST   | /pane/\<name\>/circle    | `color`, `x`, `y`, `r`                      | Yes.           | Draw a circle on a pane.                                               |
| POST   | /pane/\<name\>/line      | `color`, `x`, `y`, `x2`, `y2`               | Yes.           | Draw a line on a pane.                                                 |
| POST   | /pane/\<name\>/copy_rect | `x`, `y`, `w`, `h`, `x2`, `y2`              | Yes.           | Copy a rectangle to another place.                                     |
| POST   | /pane/\<name\>/bezier2   | `color`, `x0`, `y0`, `x1`, `y1`, `x2`, `y2` | Yes.           | Draw a quadratic bézier curve on a pane with the given control points. |
| POST   | /pane/\<name\>/triangle  | `color`, `x0`, `y0`, `x1`, `y1`, `x2`, `y2` | Yes.           | Draw a triangle on a pane.                                             |
| DELETE | /pane/\<name\>           | None.                                       | Yes.           | Delete a pane.                                                         |


### Types

Where `x`, `y`, `w`, and `h` (or `x2`, `y2`, and so forth) are given as query
parameters, integer values are expected. `x = 0` and `y = 0` refers to the top
left corner of the pane.

Where `color` is a query parameter, the value expected should be of the form
`%23XXXXXX`, where `%23` is the URL-escaped version of `#`, and `XXXXXX` is a
hexadecimal color code. That is, ttds expects a URL-escaped color hex code.