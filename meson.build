project('tssupervisorupdate', 'c', version: '1.1.0')
add_project_arguments('-DTAG="' + meson.project_version() + '"', language: 'c')
executable('tssupervisorupdate', 
  [
    'tssupervisorupdate.c',
    'micro.c',
    'update-shared.c',
    'update-v0.c',
    'update-v1.c',
	'crc8.c',
  ], 
  install : true
)
