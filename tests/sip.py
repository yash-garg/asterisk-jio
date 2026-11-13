import re
import socket
import struct
import time
import uuid

server = ("127.0.0.1", 15060)


def call(
    codec="AMR",
    rate=8000,
    fmtp="octet-align=0",
    channels="",
    expected=200,
    fallback=False,
    reinvite=None,
    extra="",
    method="INVITE",
):
    """Offer one payload to Echo(), then optionally renegotiate the dialog."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(6)
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("127.0.0.1", 0))
    rtp.settimeout(2)
    port = sock.getsockname()[1]
    rp = rtp.getsockname()[1]
    cid = str(uuid.uuid4())
    tag = uuid.uuid4().hex
    branch = "z9hG4bK" + uuid.uuid4().hex
    body = (
        f"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=offline-amr\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio {rp} RTP/AVP 97"
        + (" 0" if fallback else "")
        + f"\r\na=rtpmap:97 {codec}/{rate}{channels}\r\n"
    )
    if fmtp is not None:
        body += f"a=fmtp:97 {fmtp}\r\n"
    if fallback:
        body += "a=rtpmap:0 PCMU/8000\r\n"
    body += extra
    body += "a=sendrecv\r\na=ptime:20\r\n"
    fromh = f"<sip:validation@127.0.0.1>;tag={tag}"
    to = "<sip:100@127.0.0.1>"

    def send(
        method, uri="sip:100@127.0.0.1", seq=1, content="", destto=None, via_branch=None
    ):
        msg = f"{method} {uri} SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:{port};branch={via_branch or branch};rport\r\nMax-Forwards: 70\r\nFrom: {fromh}\r\nTo: {destto or to}\r\nCall-ID: {cid}\r\nCSeq: {seq} {method}\r\nContact: <sip:validation@127.0.0.1:{port}>\r\n"
        if content:
            msg += "Content-Type: application/sdp\r\n"
        msg += f"Content-Length: {len(content)}\r\n\r\n{content}"
        sock.sendto(msg.encode(), server)

    send("INVITE", content=body)
    while True:
        response = sock.recv(8192).decode()
        status = int(response.split()[1])
        if status >= 200:
            break
    assert status == expected, (codec, fmtp, status, response)
    toh = (
        re.search(r"^To:\s*(.*)$", response, re.MULTILINE | re.IGNORECASE)
        .group(1)
        .strip()
    )
    uri = re.search(r"^Contact:\s*<([^>]+)>", response, re.MULTILINE | re.IGNORECASE)
    uri = uri.group(1) if uri else "sip:100@127.0.0.1"
    send(
        "ACK",
        uri=uri,
        destto=toh,
        via_branch="z9hG4bK" + uuid.uuid4().hex if status == 200 else branch,
    )
    if status == 200:
        sdp = response.split("\r\n\r\n", 1)[1]
        if fallback:
            assert "a=rtpmap:97 AMR/" not in sdp and "PCMU/8000" in sdp, sdp
        else:
            assert f"{codec}/{rate}" in sdp, sdp
            align = 1 if fmtp and "octet-align=1" in fmtp else 0
            assert f"octet-align={align}" in sdp, sdp
            if fmtp and "mode-set=0,1,2" in fmtp:
                assert "mode-set=0,1,2" in sdp, sdp
        # Exercise actual RTP sample accounting and echo after SDP negotiation.
        peer_port = int(re.search(r"m=audio (\d+)", sdp).group(1))
        if not fallback:
            sample = bytes.fromhex(
                "f06d478cc30d03ece2183e282080"
                if codec == "AMR"
                else "f15e5198c564c7c50c6c82191603f00a0b5753517f97977931dd731b9254f5799a"
            )
            if align:
                bits = "".join(f"{b:08b}" for b in sample)
                speech = bits[10 : 10 + (95 if codec == "AMR" else 253)]
                speech += "0" * (-len(speech) % 8)
                packed = "11110000" + bits[4:10] + "00" + speech
                sample = int(packed, 2).to_bytes(len(packed) // 8, "big")
            received = 0
            for seq in range(12):
                packet = (
                    struct.pack("!BBHII", 0x80, 97, seq, seq * (rate // 50), 12345)
                    + sample
                )
                rtp.sendto(packet, ("127.0.0.1", peer_port))
                time.sleep(0.02)
            rtp.settimeout(0.3)
            try:
                while True:
                    echoed = rtp.recv(4096)
                    if len(echoed) > 12 and (echoed[1] & 127) == 97:
                        received += 1
            except TimeoutError:
                pass
            assert received >= 3, ("missing RTP echo", codec, align, received)
        if reinvite is not None:
            rb = body.replace("o=- 1 1 ", "o=- 1 2 ").replace(
                "a=fmtp:97 " + fmtp, "a=fmtp:97 " + reinvite
            )
            rebranch = "z9hG4bK" + uuid.uuid4().hex
            send(method, uri=uri, seq=2, content=rb, destto=toh, via_branch=rebranch)
            while True:
                reply = sock.recv(8192).decode()
                if ("2 " + method) in reply and int(reply.split()[1]) >= 200:
                    break
            code = int(reply.split()[1])
            wanted = 200 if reinvite == fmtp else 488
            assert code == wanted, ("reinvite", reinvite, code, reply)
            if method == "INVITE":
                send(
                    "ACK",
                    uri=uri,
                    seq=2,
                    destto=toh,
                    via_branch=rebranch
                    if code != 200
                    else "z9hG4bK" + uuid.uuid4().hex,
                )
            print("PASS", method, reinvite, code, flush=True)
        send("BYE", uri=uri, seq=3, destto=toh, via_branch="z9hG4bK" + uuid.uuid4().hex)
        while True:
            reply = sock.recv(8192).decode()
            if "3 BYE" in reply:
                assert reply.startswith("SIP/2.0 200")
                break
    print(
        "PASS",
        codec,
        rate,
        fmtp,
        channels,
        "fallback" if fallback else "",
        status,
        flush=True,
    )
    sock.close()
    rtp.close()


def run():
    for codec, rate in [("AMR", 8000), ("AMR-WB", 16000)]:
        for align in [0, 1]:
            call(
                codec,
                rate,
                f"octet-align={align};mode-set=0,1,2;mode-change-period=2;mode-change-neighbor=1",
            )
        call(codec, rate, fmtp=None)
    for value in [
        "octet-align=2",
        "crc=1",
        "robust-sorting=1",
        "interleaving=0",
        "mode-set=8",
        "mode-set=0,",
        "mode-change-period=3",
    ]:
        call(fmtp=value, expected=488)
    call(channels="/2", expected=488)
    call(fmtp="crc=1", fallback=True)
    call(fmtp="octet-align=1;" + "x=0;" * 80, expected=488)
    for method in ["INVITE", "UPDATE"]:
        for value in ["octet-align=0", "crc=1"]:
            call(reinvite=value, method=method)
    call(extra="a=fmtp:97 crc=1\r\n", expected=488)
    call(extra="a=rtpmap:97 AMR/8000/2\r\n", expected=488)
