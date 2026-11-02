custom_target(
  'termshare.1',
  input: 'termshare.xml',
  output: 'termshare.1',
  command: [xsltproc, '--nonet', '--output', '@OUTPUT@', 'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl', '@INPUT@'],
  install: true,
  install_dir: get_option('prefix') / get_option('mandir') / 'man1',
)
