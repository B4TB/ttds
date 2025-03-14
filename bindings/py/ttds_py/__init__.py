from urllib.parse import urlunparse, urlencode, quote
from dataclasses import dataclass, field
from requests import request, exceptions
from typing import Optional

class DuplicatePaneError(Exception):
    name: Optional[str]

    def __init__(self, name: Optional[str] = None):
        self.name = name

    def __str__(self):
        if self.name is not None:
            return f"The name \"{self.name}\" is not unique."
        else:
            return "Pane already exists."

class UnAuthError(Exception):
    def __str__(self):
        return "Authorization for the request failed. The quickest way to resolve this is likely to recreate the pane."

class NoAuthError(Exception):
    def __str__(self):
        return "A route expecting an Auth header either didn't get one, or it was malformed. This is almost certainly a bug in the Python bindings."

@dataclass
class Color:
    r: int
    g: int
    b: int

    def __init__(self, r: int, g: int, b: int):
        if r not in range(0, 256) or g not in range(0, 256) or b not in range(0, 256):
            raise ValueError("Color values must be at most 255.")

        self.r = r
        self.g = g
        self.b = b

    def __str__(self):
        return f"#{self.r:02x}{self.g:02x}{self.b:02x}"


class Connection:
    _host: str
    _use_tls: bool

    def __init__(self, host: str, use_tls: bool = False):
        self._host = host
        self._use_tls = use_tls

    def request(
        self, *args, method: Optional[str] = None, auth: Optional[str] = None, **kwargs
    ) -> str:
        escape = lambda x: quote(str(x), safe="")
        route = "/".join(escape(arg) for arg in args)
        query = urlencode(kwargs)

        scheme = "https" if self._use_tls else "http"
        target = urlunparse((scheme, self._host, route, "", query, ""))

        headers = None if auth is None else {"Auth": auth}

        try:
            r = request(method or "POST", target, headers=headers)
        except exceptions.ConnectionError as e:
            e.add_note(f"\033[31;1mA request made by this connection object to {scheme}://{self._host} failed.\033[0m")
            e.add_note("It's likely that this connection object is misconfigured or that the server is down.")
            e.add_note("Ask for help!")
            raise

        if r.status_code == 409:
            raise DuplicatePaneError()
        elif r.status_code == 401:
            raise NoAuthError()
        elif r.status_code == 403:
            raise UnAuthError()

        try:
            r.raise_for_status()
        except exceptions.HTTPError as e:
            e.add_note("An unexpected HTTP error code came up. This is likely a bug in the Python bindings.")
            raise

        return str(r.content, "utf-8")


@dataclass
class Pane:
    name: str
    color: Color
    conn: Connection

    _token: Optional[str] = field(default=None)  # TODO: Make this a UUIDv4.

    def __enter__(self):
        try:
            self._token = self.conn.request("pane", self.name, "create", color=self.color)
        except DuplicatePaneError as e:
            e.name = self.name
            raise

        return self

    def __exit__(self, exc_type, exc_val, exc_traceback):
        assert self._token is not None
        self.conn.request("pane", self.name, auth=self._token, method="DELETE")

    def rect(self, x: int, y: int, w: int, h: int, color: Color):
        self._draw("rect", x=x, y=y, w=w, h=h, color=color)

    def circle(self, x: int, y: int, r: int, color: Color):
        self._draw("circle", x=x, y=y, r=r, color=color)

    def line(self, x: int, y: int, x2: int, y2: int, color: Color):
        self._draw("line", x=x, y=y, x2=x2, y2=y2, color=color)

    def copy_rect(self, x: int, y: int, w: int, h: int, x2: int, y2: int):
        self._draw("copy_rect", x=x, y=y, w=w, h=h, x2=x2, y2=y2)

    def bezier2(self, x0, y0, x1, y1, x2, y2, color: Color):
        self._draw("bezier2", x0=x0, y0=y0, x1=x1, y1=y1, x2=x2, y2=y2, color=color)

    def triangle(self, x0, y0, x1, y1, x2, y2, color: Color):
        self._draw("triangle", x0=x0, y0=y0, x1=x1, y1=y1, x2=x2, y2=y2, color=color)

    def _draw(self, shape, **kwargs):
        assert self._token is not None
        self.conn.request("pane", self.name, shape, auth=self._token, **kwargs)
