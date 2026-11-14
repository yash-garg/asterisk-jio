def configure(base, package, addon, port, jio_port):
    for d in ["etc", "modules", "var", "log", "run", "spool", "cache"]:
        (base / d).mkdir(parents=True, exist_ok=True)
    for module in (package / "lib/asterisk/modules").glob("*.so"):
        link = base / "modules" / module.name
        if link.is_symlink():
            link.unlink()
        link.symlink_to(module)
    validation = base / "modules/amr_validation.so"
    if validation.is_symlink():
        validation.unlink()
    validation.symlink_to(str(addon / "amr_validation.so"))
    (base / "etc/asterisk.conf").write_text(f"""[directories]
    astetcdir => {base}/etc
    astmoddir => {base}/modules
    astvarlibdir => {base}/var
    astdbdir => {base}/var
    astkeydir => {base}/var
    astdatadir => {package}/var/lib/asterisk
    astagidir => {base}/var
    astspooldir => {base}/spool
    astrundir => {base}/run
    astlogdir => {base}/log
    astcachedir => {base}/cache
    [options]
    nofork = yes
    verbose = 0
    debug = 0
    """)
    modules = [
        "res_sorcery_config",
        "res_sorcery_memory",
        "res_sorcery_astdb",
        "res_timing_timerfd",
        "codec_amr",
        "codec_ulaw",
        "codec_alaw",
        "codec_resample",
        "res_pjproject",
        "res_pjsip",
        "res_pjsip_amr",
        "res_pjsip_jio",
        "res_pjsip_outbound_authenticator_digest",
        "res_pjsip_outbound_registration",
        "jio_contact_test",
        "res_pjsip_session",
        "res_pjsip_pubsub",
        "res_pjsip_sdp_rtp",
        "res_rtp_asterisk",
        "res_pjsip_endpoint_identifier_ip",
        "chan_pjsip",
        "pbx_config",
        "app_echo",
        "res_clioriginate",
        "res_pjsip_authenticator_digest",
        "amr_validation",
    ]
    (base / "etc/modules.conf").write_text(
        "[modules]\nautoload=no\n" + "".join(f"require={m}.so\n" for m in modules)
    )
    (base / "etc/logger.conf").write_text(
        "[logfiles]\nconsole=notice,warning,error\nvalidation=notice,warning,error\n"
    )
    (base / "etc/pjsip.conf").write_text(f"""[local]
    type=transport
    protocol=udp
    bind=127.0.0.1:{port}
    [validation]
    type=endpoint
    context=validate
    disallow=all
    allow=amr,amrwb,ulaw,alaw
    direct_media=no
    [loopback]
    type=identify
    endpoint=validation
    match=127.0.0.1
    """)
    (base / "etc/extensions.conf").write_text(
        "[validate]\nexten => 100,1,Answer()\n same => n,Echo()\n same => n,Hangup()\n"
    )
    (base / "etc/rtp.conf").write_text(
        "[general]\nrtpstart=53000\nrtpend=53100\nicesupport=no\n"
    )
    # Dummy credentials and a loopback-only registrar; never contact the carrier.
    (base / "etc/pjsip_jio.conf").write_text("""[general]
    realm=example.invalid
    username=911234567890@example.invalid
    digest_uri=sip:example.invalid
    access_network=IEEE-802.3
    instance_id=00000000-0000-1000-8000-000000000001
    """)
    with (base / "etc/pjsip.conf").open("a") as config:
        config.write(f"""
    [offline-auth]
    type=auth
    auth_type=userpass
    username=911234567890@example.invalid
    password=offline-test-only
    realm=example.invalid
    [offline-registration]
    type=registration
    transport=local
    outbound_auth=offline-auth
    server_uri=sip:example.invalid
    client_uri=sip:+911234567890@example.invalid
    contact_user=+911234567890
    outbound_proxy=sip:127.0.0.1:{jio_port}\\;lr
    expiration=3600
    max_random_initial_delay=0
    max_retries=0
    """)

    for filename in [
        "cdr",
        "cel",
        "features",
        "acl",
        "udptl",
        "indications",
        "ccss",
        "pjproject",
    ]:
        (base / "etc" / f"{filename}.conf").write_text(
            (package / "etc/asterisk" / f"{filename}.conf").read_text()
        )
    (base / "etc/manager.conf").write_text("[general]\nenabled=no\n")

    for name in ["codec_amr", "res_pjsip_amr", "res_pjsip_jio", "jio_contact_test"]:
        target = base / "modules" / f"{name}.so"
        target.unlink(missing_ok=True)
        target.symlink_to(addon / f"{name}.so")
    return base / "etc/asterisk.conf"
