"""Exercise real REGISTER authentication against a localhost-only registrar."""

import hashlib
import re
import socket
import subprocess
import time

from outbound import header

USERNAME = "911234567890@example.invalid"
PASSWORD = "offline-test-only"
REALM = "example.invalid"


def registrar():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(5)
    return sock


def digest(value):
    return {
        name.lower(): value.strip('"')
        for name, value in re.findall(r'(\w+)=("[^"]*"|[^,\s]+)', value)
    }


def run(sock, cli):
    def receive(reason="REGISTER", *, allow_unregister=False):
        try:
            message, peer = sock.recvfrom(16384)
        except TimeoutError as error:
            raise AssertionError(f"No {reason} from Asterisk") from error
        message = message.decode()
        assert message.startswith("REGISTER sip:example.invalid SIP/2.0"), message
        expiry = int(header(message, "Expires"))
        assert expiry > 0 or (allow_unregister and expiry == 0), message
        assert header(message, "P-Access-Network-Info") == "IEEE-802.3"
        contact = header(message, "Contact")
        assert ";+u.jio.jfv" in contact and ";q=0.5" in contact, contact
        assert '+sip.instance="<00000000-0000-1000-8000-000000000001>"' in contact
        assert "+g.3gpp.icsi-ref" in contact and "+g.3gpp.iari-ref" in contact
        return message, peer

    def respond(request, status, extra):
        message, peer = request
        response = f"SIP/2.0 {status}\r\n"
        for name in ["Via", "From", "To", "Call-ID", "CSeq"]:
            value = header(message, name)
            if name == "To" and ";tag=" not in value:
                value += ";tag=offline-registrar"
            response += f"{name}: {value}\r\n"
        response += extra + "Content-Length: 0\r\n\r\n"
        sock.sendto(response.encode(), peer)

    def challenge(
        request,
        nonce,
        *,
        status="401 Unauthorized",
        stale=None,
        realm=REALM,
        retry_after=None,
        qop="auth",
    ):
        name = "Proxy-Authenticate" if status.startswith("407") else "WWW-Authenticate"
        extra = f'{name}: Digest realm="{realm}", nonce="{nonce}", algorithm=MD5'
        if qop is not None:
            extra += f', qop="{qop}"'
        if stale is not None:
            extra += f", stale={str(stale).lower()}"
        extra += "\r\n"
        if retry_after is not None:
            extra += f"Retry-After: {retry_after}\r\n"
        respond(request, status, extra)

    def authenticated(previous, nonce, *, proxy=False, qop="auth"):
        request = receive("authenticated REGISTER for the fresh challenge")
        message = request[0]
        name = "Proxy-Authorization" if proxy else "Authorization"
        auth = digest(header(message, name))
        md5 = lambda text: hashlib.md5(text.encode(), usedforsecurity=False).hexdigest()
        # Independently calculate the wire Digest, including qop/nc/cnonce.
        ha1 = md5(f"{USERNAME}:{REALM}:{PASSWORD}")
        ha2 = md5("REGISTER:sip:example.invalid")
        if qop is None:
            expected = md5(f"{ha1}:{nonce}:{ha2}")
            assert "qop" not in auth, auth
        else:
            expected = md5(f"{ha1}:{nonce}:{auth['nc']}:{auth['cnonce']}:auth:{ha2}")
            assert auth["qop"] == "auth" and auth["nc"] == "00000001", auth
        assert auth["username"] == USERNAME and auth["realm"] == REALM, auth
        assert auth["nonce"] == nonce and auth["response"] == expected, auth
        assert len(re.findall(rf"^{name}:", message, re.MULTILINE | re.IGNORECASE)) == 1
        assert int(header(message, "CSeq").split()[0]) > int(
            header(previous[0], "CSeq").split()[0]
        )
        return request

    def state_is(wanted, *, minimum_delay=0):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = subprocess.check_output(
                cli + ["pjsip show registrations"], text=True, timeout=5
            )
            match = re.search(rf"offline-registration/.*\b{wanted}\b(.*)", state)
            if match:
                expiry = re.search(r"\(exp\. (\d+)s\)", match[1])
                if not minimum_delay or (expiry and int(expiry[1]) >= minimum_delay):
                    return
            time.sleep(0.05)
        raise AssertionError(f"Local registration did not reach {wanted}: {state}")

    def quiet(seconds=0.3):
        old_timeout = sock.gettimeout()
        sock.settimeout(seconds)
        try:
            message, _ = sock.recvfrom(16384)
        except TimeoutError:
            return
        finally:
            sock.settimeout(old_timeout)
        raise AssertionError(f"Unexpected extra REGISTER: {message.decode()}")

    def reject():
        state_is("Rejected")
        quiet()

    def start(nonce, *, status="401 Unauthorized", qop="auth"):
        subprocess.run(
            cli + ["pjsip send register offline-registration"],
            check=True,
            capture_output=True,
            timeout=5,
        )
        # The CLI unregisters an existing binding before starting a new round.
        request = receive(allow_unregister=True)
        if int(header(request[0], "Expires")) == 0:
            respond(request, "200 OK", "Expires: 0\r\n")
            request = receive("registration after CLI unregistration")
        challenge(request, nonce, status=status, qop=qop)
        return authenticated(request, nonce, proxy=status.startswith("407"), qop=qop)

    def accept(request, expiry=3600):
        respond(
            request,
            "200 OK",
            f"Contact: {header(request[0], 'Contact')};expires={expiry}\r\nExpires: {expiry}\r\n",
        )
        # A renewal may still display Registered from its previous binding.
        # The CLI countdown is the next refresh (ten seconds before expiry).
        # For long bindings, wait for that timer to advance as well.
        state_is("Registered", minimum_delay=max(0, expiry - 15))

    first = receive()
    assert digest(header(first[0], "Authorization")) == {
        "username": USERNAME,
        "realm": REALM,
        "nonce": "",
        "uri": "sip:example.invalid",
        "response": "",
    }
    challenge(first, "initial", status="483 Too Many Hops")
    request = authenticated(first, "initial")
    # Regression: the old core abandons this valid replacement challenge.
    # max_retries=0 ensures a scheduled retry cannot mask that failure.
    for nonce in ["stale-one", "stale-two"]:
        challenge(request, nonce, stale=True)
        request = authenticated(request, nonce)
    accept(request, expiry=12)  # Core renews ten seconds before expiry.
    print("PASS Jio initial auth and two fresh stale-nonce challenges", flush=True)

    renewal = receive("automatic registration renewal")
    challenge(renewal, "renewal")
    request = authenticated(renewal, "renewal")
    for nonce in ["renewal-stale-one", "renewal-stale-two"]:
        challenge(request, nonce, stale=True)
        request = authenticated(request, nonce)
    accept(request)
    print("PASS stale-challenge budget resets for automatic renewal", flush=True)

    request = start("without-qop", qop=None)
    challenge(request, "without-qop-fresh", stale=True, qop=None)
    request = authenticated(request, "without-qop-fresh", qop=None)
    accept(request)
    print("PASS Digest without qop retains stale-challenge support", flush=True)

    request = start("proxy-initial", status="407 Proxy Authentication Required")
    challenge(
        request, "proxy-fresh", status="407 Proxy Authentication Required", stale=True
    )
    request = authenticated(request, "proxy-fresh", proxy=True)
    accept(request)
    print("PASS 407 stale challenge uses Proxy-Authorization", flush=True)

    # These must remain failures; stale handling must not bypass ordinary
    # authentication rejection or answer an unrelated/unchanged challenge.
    cases = [
        ("ordinary rejection", "different", {}),
        ("stale=false", "different", {"stale": False}),
        ("unchanged nonce", "negative", {"stale": True}),
        ("empty nonce", "", {"stale": True}),
        ("unrelated realm", "different", {"stale": True, "realm": "other.invalid"}),
    ]
    for label, nonce, options in cases:
        request = start("negative")
        challenge(request, nonce, **options)
        reject()
        print(f"PASS rejects {label} without another authenticated request", flush=True)

    request = start("limit-initial")
    for nonce in ["limit-one", "limit-two"]:
        challenge(request, nonce, stale=True)
        request = authenticated(request, nonce)
    challenge(request, "limit-three", stale=True)
    reject()
    print(
        "PASS repeated fresh stale challenges stop after two extra attempts", flush=True
    )

    # A Retry-After instruction must retain the existing scheduled-retry path.
    request = start("delay-initial")
    challenge(request, "delay-fresh", stale=True, retry_after=2)
    quiet(seconds=1)
    scheduled = receive("scheduled Retry-After registration")
    challenge(scheduled, "scheduled-initial")
    request = authenticated(scheduled, "scheduled-initial")
    for nonce in ["scheduled-one", "scheduled-two"]:
        challenge(request, nonce, stale=True)
        request = authenticated(request, nonce)
    accept(request)
    print(
        "PASS Retry-After is respected and scheduled retry gets a fresh budget",
        flush=True,
    )
