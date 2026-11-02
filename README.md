[![License](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://github.com/jamesd-comelang/termshare/blob/master/COPYING)

# termshare

Termshare is a lightweight and easy to use terminal emulator application
with many advanced features including drop down, tabs, unlimited scrolling,
full colors, fonts, transparent backgrounds, and more.

----

### Homepage

[Termshare documentation](https://github.com/jamesd-comelang/termshare)

### Changelog

See [NEWS](https://github.com/jamesd-comelang/termshare/blob/master/NEWS) for details on changes and fixes made in the current release.

### Performance Issues

Termshare is based on the Vte terminal widget library, just like
gnome-terminal. Vte is probably not the fastest terminal emulation library on
earth, but it's one of the best when it comes to Unicode support, and not to
forget, it's actively developed.

### Source Code Repository

[Termshare source code](https://github.com/jamesd-comelang/termshare)

### Download a Release Tarball

[Termshare archive](https://github.com/jamesd-comelang/termshare)
    or
[Termshare tags](https://github.com/jamesd-comelang/termshare/tags)

### Installation

From source code repository: 

    % cd termshare
    % meson setup build
    % meson compile -C build
    % meson install -C build

From release tarball:

    % tar xf termshare-<version>.tar.xz
    % cd termshare-<version>
    % meson setup build
    % meson compile -C build
    % meson install -C build

### Uninstallation

    % ninja uninstall -C build

### Reporting Bugs

Visit the [reporting bugs](https://github.com/jamesd-comelang/termshare/issues) page to view currently open bug reports and instructions on reporting new bugs or submitting bugfixes.

