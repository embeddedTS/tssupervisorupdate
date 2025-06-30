project('tssupervisorupdate', 'c')

# Uses versioning based on tag.  Eg:
# For tag v0.1.0:
# 1) If we match the exact tag: 0.1.0
# 2) Commits since tag: 1.1.4+git1
# 3) For tarball: '0.0.0+unknown'

git_desc = run_command(
    ['git', 'describe', '--tags', '--match', 'v[0-9]*',
     '--long', '--always', '--dirty=_dirty'],
    check : false,
    capture : true
).stdout().strip()

if git_desc.startswith('v')
  git_desc = git_desc.substring(1)
endif

parts = git_desc.split('-')
if parts.length() == 1
  ver = parts[0]
elif parts[1] == '0'
  ver = parts[0]
else
  ver = '@0@+git@1@'.format(parts[0], parts[1])
endif

if ver == ''
  ver = '0.0.0+unknown'
endif

message('Building version ' + ver)

add_project_arguments('-DTAG="@0@"'.format(ver), language : 'c')
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
