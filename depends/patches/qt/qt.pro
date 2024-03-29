# Create the super cache so modules will add themselves to it.
cache(, super)

!QTDIR_build: cache(CONFIG, add, $$list(QTDIR_build))

prl = no_install_prl
CONFIG += $$prl
cache(CONFIG, add stash, prl)

TEMPLATE = subdirs
SUBDIRS = qtbase qtdeclarative qtgraphicaleffects qtquickcontrols qtquickcontrols2 qttools qttranslations

qtdeclarative.depends = qtbase
qtgraphicaleffects.depends = qtdeclarative
qtquickcontrols.depends = qtdeclarative
qtquickcontrols2.depends = qtgraphicaleffects
qttools.depends = qtbase
qttranslations.depends = qttools

load(qt_configure)
