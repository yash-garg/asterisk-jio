import re
import socket
import subprocess
import time
import uuid

CLI = []
PORT = 0


def header(message, name):
    return (
        re.search("^" + name + r":\s*(.*)$", message, re.IGNORECASE | re.MULTILINE)
        .group(1)
        .strip()
    )


def test(params, expected_bye, provisional=False, reliable=False, codec="AMR"):
    global PORT
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(8)
    PORT = sock.getsockname()[1]
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("127.0.0.1", 0))
    proc = subprocess.Popen(
        CLI
        + [
            f"channel originate PJSIP/validation/sip:test@127.0.0.1:{PORT} application Echo"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    invite, peer = sock.recvfrom(16384)
    invite = invite.decode()
    assert invite.startswith("INVITE "), invite
    rate = 16000 if codec == "AMR-WB" else 8000
    pt = re.search(rf"a=rtpmap:(\d+) {codec}/{rate}", invite).group(1)
    toh = header(invite, "To") + ";tag=" + uuid.uuid4().hex
    fromh = header(invite, "From")
    cid = header(invite, "Call-ID")
    sdp = f"v=0\r\no=- 4 4 IN IP4 127.0.0.1\r\ns=addon-test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio {rtp.getsockname()[1]} RTP/AVP {pt}\r\na=rtpmap:{pt} {codec}/{rate}\r\na=fmtp:{pt} {params}\r\na=sendrecv\r\n"

    def respond(request, status, body="", tagged_to=None, extra=""):
        msg = f"SIP/2.0 {status}\r\nVia: {header(request, 'Via')}\r\nFrom: {header(request, 'From')}\r\nTo: {tagged_to or header(request, 'To')}\r\nCall-ID: {header(request, 'Call-ID')}\r\nCSeq: {header(request, 'CSeq')}\r\nContact: <sip:test@127.0.0.1:{PORT}>\r\n"
        if body:
            msg += "Content-Type: application/sdp\r\n"
        msg += extra
        msg += f"Content-Length: {len(body)}\r\n\r\n{body}"
        sock.sendto(msg.encode(), peer)

    respond(invite, "100 Trying", tagged_to=toh)
    respond(
        invite,
        "183 Session Progress" if provisional else "200 OK",
        sdp,
        toh,
        "Require: 100rel\r\nRSeq: 1\r\n" if reliable else "",
    )
    bye = False
    ack = False
    prack = False
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        sock.settimeout(max(0.01, deadline - time.monotonic()))
        try:
            msg = sock.recv(16384).decode()
        except TimeoutError:
            break
        if msg.startswith("PRACK "):
            prack = True
            respond(msg, "200 OK")
            if not expected_bye:
                respond(invite, "200 OK", sdp, toh)
        elif msg.startswith("ACK "):
            ack = True
            if not expected_bye:
                break
        elif msg.startswith("BYE "):
            bye = True
            respond(msg, "200 OK")
            break
        elif msg.startswith("CANCEL "):
            bye = True
            respond(msg, "200 OK")
            respond(invite, "487 Request Terminated", tagged_to=toh)
            break
    if not bye:
        uri = re.search(
            r"^Contact:\s*<([^>]+)>", invite, re.MULTILINE | re.IGNORECASE
        ).group(1)
        branch = "z9hG4bK" + uuid.uuid4().hex
        msg = f"BYE {uri} SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:{PORT};branch={branch}\r\nMax-Forwards: 70\r\nFrom: {toh}\r\nTo: {fromh}\r\nCall-ID: {cid}\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n"
        sock.sendto(msg.encode(), peer)
    proc.communicate(timeout=10)
    sock.close()
    rtp.close()
    assert bye == expected_bye, (
        params,
        "provisional",
        provisional,
        "terminated",
        bye,
        "ACK",
        ack,
    )
    if not expected_bye:
        assert ack, "Valid answer was never acknowledged"
        if reliable:
            assert prack, "Reliable provisional response was never acknowledged"
    print(
        "PASS outbound",
        codec,
        params,
        "183" if provisional else "200",
        "terminated" if bye else "accepted",
        flush=True,
    )


def run(cli, port):
    global CLI, PORT
    CLI, PORT = cli, port
    test("octet-align=0", False)
    test("crc=1", True)
    test("crc=1", True, True)
    test("octet-align=1", True)
    test("octet-align=0;mode-set=0,2", False)
    test("octet-align=0", False, codec="AMR-WB")
    test("octet-align=1", True, codec="AMR-WB")
    test("octet-align=0", False, provisional=True, reliable=True)
    test("crc=1", True, provisional=True, reliable=True)
