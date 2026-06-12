sources = files(
  'main.c',
  'config.c',
  'usrp_protocol.c',
  'usrp_transport.c',
  'audio_alsa.c',
  'audio_processing.c',
  'audio_trim.c',
  'logic_hid.c',
  'jitter_buffer.c',
  'telemetry.c',
  'watchdog.c',
)

executable('rusrp',
  sources,
  dependencies : [
    alsa_dep,
    sd_dep,
    thread_dep,
    m_dep,
    tomlc99_dep,
  ],
  install : true,
)
