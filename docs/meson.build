# Manual pages.
#
# The *.man.in files are pregenerated from the pods and checked in. Adapt
# resource names and fill in the @@...@@ placeholders here.

sed = find_program('sed')

sed_args = [
  '-e', 's%@@RXVT_VERSION@@%' + version + '%g',
  '-e', 's%@@RXVT_NAME@@%' + rxvtname + '%g',
  '-e', 's%@@RXVT_DATE@@%' + date + '%g',
  '-e', 's%@@RXVT_LIBDIR@@%' + libdir + '%g',
  '-e', 's%@@URXVT_NAME@@%urxvt%g',
  # Resource examples and class names must match the configured class.
  '-e', 's%URxvt[.]%' + get_option('res-class') + '.%g',
  '-e', 's%fBURxvt%fB' + get_option('res-class') + '%g',
]

# [source, installed name, section]
man_pages = [
  ['rxvt.1.man.in', rxvtname, '1'],
  ['rxvt.7.man.in', rxvtname, '7'],
]

foreach page : man_pages
  custom_target(page[1] + '.' + page[2],
    input: page[0],
    output: page[1] + '.' + page[2],
    command: [sed, sed_args, '@INPUT@'],
    capture: true,
    install: true,
    install_dir: mandir / 'man' + page[2],
  )
endforeach

# Terminfo installation is disabled as a workaround for FS#79143.
