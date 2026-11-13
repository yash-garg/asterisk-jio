"""Exercise the real outbound registration path against a localhost registrar."""

import hashlib
import re
import socket
import subprocess
import time

from outbound import header

USERNAME = "911234567890@example.invalid"
PASSWORD = "offline-test-only"
NONCE = "0123456789abcdef"


def registrar():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(10)
    return sock


def digest(value):
    return dict(re.findall(r'(\w+)="([^"]*)"', value))


def run(sock, cli):
    def receive():
        message, peer = sock.recvfrom(16384)
        message = message.decode()
        assert message.startswith("REGISTER sip:example.invalid SIP/2.0"), message
        assert header(message, "P-Access-Network-Info") == "IEEE-802.3"
        contact = header(message, "Contact")
        assert ";+u.jio.jfv" in contact and ";q=0.5" in contact, contact
        assert '+sip.instance="<00000000-0000-1000-8000-000000000001>"' in contact
        assert "+g.3gpp.icsi-ref" in contact and "+g.3gpp.iari-ref" in contact
        return message, peer

    def respond(message, peer, status, extra):
        response = f"SIP/2.0 {status}\r\n"
        for name in ["Via", "From", "To", "Call-ID", "CSeq"]:
            value = header(message, name)
            if name == "To" and ";tag=" not in value:
                value += ";tag=offline-registrar"
            response += f"{name}: {value}\r\n"
        response += extra + "Content-Length: 0\r\n\r\n"
        sock.sendto(response.encode(), peer)

    first, peer = receive()
    auth = digest(header(first, "Authorization"))
    assert auth == {
        "username": USERNAME,
        "realm": "example.invalid",
        "nonce": "",
        "uri": "sip:example.invalid",
        "response": "",
    }, auth
    respond(
        first,
        peer,
        "483 Too Many Hops",
        f'WWW-Authenticate: Digest realm="example.invalid", nonce="{NONCE}", algorithm=MD5\r\n',
    )
    second, peer = receive()
    auth = digest(header(second, "Authorization"))
    md5 = lambda text: hashlib.md5(text.encode(), usedforsecurity=False).hexdigest()
    expected = md5(
        f"{md5(f'{USERNAME}:example.invalid:{PASSWORD}')}:{NONCE}:{md5('REGISTER:sip:example.invalid')}"
    )
    assert auth["nonce"] == NONCE and auth["response"] == expected, auth
    assert (
        len(re.findall(r"^Authorization:", second, re.MULTILINE | re.IGNORECASE)) == 1
    )
    assert int(header(second, "CSeq").split()[0]) > int(
        header(first, "CSeq").split()[0]
    )
    respond(
        second,
        peer,
        "200 OK",
        f"Contact: {header(second, 'Contact')};expires=3600\r\nExpires: 3600\r\n",
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        state = subprocess.check_output(
            cli + ["pjsip show registrations"], text=True, timeout=5
        )
        if re.search(r"offline-registration/.*\bRegistered\b", state):
            break
        time.sleep(0.05)
    else:
        raise AssertionError(f"Local registration did not reach Registered: {state}")
    print(
        "PASS Jio empty Digest, 483 challenge, authenticated retry, IMS headers and Contact",
        flush=True,
    )
