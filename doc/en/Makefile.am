# automake doesn't play nicely with DocBook :(

SUBDIRS=images

EXTRA_DIST=aptitude.xml manpage.xml

# Put documentation in /usr/share/doc/aptitude (by default)
docdir=$(datadir)/doc/$(PACKAGE)

# Automake automagically distributes README without asking..
README=README.en

HTML2TEXT=$(srcdir)/../html-to-text

IMAGES = $(wildcard $(srcdir)/images/*.png)

man_MANS=aptitude.8 aptitude-create-state-bundle.1 aptitude-run-state-bundle.1

all-local: doc-stamp

clean-local:
	-rm -fr output-fo/ output-html/ output-txt/ output-man/
	-rm -f doc-stamp doc-css-stamp doc-fo-stamp doc-html-stamp doc-txt-stamp man-stamp
	-rm -fr aptitude.8 aptitude-create-state-bundle.1 aptitude-run-state-bundle.1
	-rm -fr $(README)

install-data-hook:
	$(mkinstalldirs) $(DESTDIR)$(docdir)/html/en/images
	$(INSTALL_DATA) output-html/*.html output-html/*.css $(DESTDIR)$(docdir)/html/en
	$(INSTALL_DATA) output-html/images/* $(DESTDIR)$(docdir)/html/en/images

	$(INSTALL_DATA) README.en $(DESTDIR)$(pkgdatadir)/README

doc-stamp: doc-html-stamp doc-css-stamp $(README) man-stamp
	touch doc-stamp

fo: doc-fo-stamp

man-stamp: $(srcdir)/aptitude.xml $(srcdir)/manpage.xml $(srcdir)/../aptitude-man.xsl
	-rm -fr output-man
	xsltproc -o output-man/ $(srcdir)/../aptitude-man.xsl $(srcdir)/aptitude.xml
	for x in $(man_MANS); do mv output-man/$$x .; done
	touch man-stamp

aptitude.8 aptitude-create-state-bundle.1 aptitude-run-state-bundle.1: man-stamp

$(README): $(srcdir)/aptitude.xml $(srcdir)/manpage.xml $(srcdir)/../aptitude-txt.xsl $(srcdir)/../aptitude-common.xsl
	-rm -fr output-txt/
	xsltproc -o output-txt/index.html $(srcdir)/../aptitude-txt.xsl $(srcdir)/aptitude.xml
	$(HTML2TEXT) output-txt/index.html | $(srcdir)/../fixup-text > $(README)

doc-fo-stamp: $(srcdir)/aptitude.xml $(srcdir)/manpage.xml $(srcdir)/../aptitude-fo.xsl $(srcdir)/../aptitude-common.xsl $(IMAGES)
	-rm -fr output-fo/
	xsltproc -o output-fo/aptitude.fo $(srcdir)/../aptitude-fo.xsl $(srcdir)/aptitude.xml

	mkdir output-fo/images/
	ln -f $(srcdir)/images/* output-fo/images/
	for x in caution draft important note tip warning; do ln -s /usr/share/xml/docbook/stylesheet/nwalsh/images/$$x.png output-fo/images/; done
	touch doc-fo-stamp

doc-pdf: doc-fo-stamp
	fop output-fo/aptitude.fo -pdf output-fo/aptitude.pdf

doc-css-stamp: doc-html-stamp $(srcdir)/../aptitude.css
	rm -f output-html/aptitude.css
	cp $(srcdir)/../aptitude.css output-html/
	touch doc-css-stamp

doc-html-stamp: aptitude.xml manpage.xml $(srcdir)/../aptitude-html.xsl $(srcdir)/../aptitude-common.xsl $(IMAGES)
	-rm -fr output-html/

	xsltproc -o output-html/ $(srcdir)/../aptitude-html.xsl $(srcdir)/aptitude.xml

	mkdir output-html/images/
	ln -f $(srcdir)/images/*.png output-html/images/
	set -e; for x in $(srcdir)/images/*.svg; do rsvg-convert -x 1.5 -y 1.5 -f png -o output-html/images/$$(basename $${x%.svg}.png) $$x; done
	for x in caution important note tip warning; do ln -s /usr/share/xml/docbook/stylesheet/nwalsh/images/$$x.png output-html/images/; done
	for x in home next prev up; do ln -s /usr/share/xml/docbook/stylesheet/nwalsh/images/$$x.gif output-html/images/; done

	touch doc-html-stamp
