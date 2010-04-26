# Copyright (C) 2010 Daniel Burrows
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
# MA 02111-1307, USA.


# scons recipies for aptitude's documentation.
#
# aptitude documentation translations are stored in doc/$LANG.
# Standalone translations contain "aptitude.xml" and/or "manpage.xml",
# with the former including the latter; po4a-based translations place
# a pofile in doc/po4a/po/$LANG.po, and they might also create an
# addendum.1.$LANG file in doc/po4a/$LANG/.
#
# This file contains canned code to generate all the expected
# documentation goodness given just the language code and a few
# parameters.

from SCons.Script import Copy, Delete, Dir, File
import os.path

def exists():
    return True

def generate(env):
    env.AddMethod(AptitudeStandaloneDocs)
    env.AddMethod(AptitudePo4aDocs)

def AptitudeStandaloneDocs(env,
                           lang,
                           output_html = 'output-html',
                           output_txt  = None,
                           output_man  = None,
                           manpage_postprocess = None,
                           temp_txt    = 'output-txt',
                           temp_man    = 'output-man',
                           mainfile = 'aptitude.xml',
                           manpage  = 'manpage.xml',
                           images   = 'images',
                           dist_xmls = True):
    '''Recipe for creating documentation for aptitude from a
standalone .xml file.

output_html is the HTML output directory.

output_txt is the output text documentation (defaults to
README.$LANG).

output_man is the output manpage (defaults to aptitude.$LANG.8).

manpage_postprocess is a function that takes the working directory and
returns a command to invoke after the manpage is fully built.  It is
provided as part of porting the legacy documentation build system;
some of the languages want to run a postprocessing script on the
manpage.

temp_txt is the temporary directory used to build the text
documentation.

temp_man is the temporary directory used to build the manpage.

mainfile is the main XML file (defaults to aptitude.xml), or None if
not present.

manpage is the manpage's XML file (defaults to manpage.xml), or None
if not present.  If mainfile is None, this is assumed to be a
standalone XML file.

images is the directory containing images to go with the HTML
documentation, or None if not present.

Set dist_xmls to False to keep the XML files from automatically being
included in the source archive (e.g., because they're autogenerated).


The input XML files and the images directory are implicitly added to
the set of distributed files.  The HTML documentation is implicitly
installed into $PKGDOCDIR/html/$LANG, the manpage is implicitly
installed to the appropriate localized manpage directory, and the text
documentation is installed to the package data directory.


The aliases \'doc-html\', \'doc-text\', and \'doc-man\' are defined to
build the HTML, text, and manpage documentation respectively.'''

    if output_txt is None:
        output_txt = 'README.%s' % lang
    if output_man is None:
        output_man = 'aptitude.%s.8' % lang

    # Register the source files for distribution.
    if dist_xmls:
        if mainfile is not None:
            env.Dist(mainfile)
        if manpage is not None:
            env.Dist(manpage)
    if images is not None:
        env.Dist(images)

    outputs = []
    fixman = []
    if manpage_postprocess is not None:
        fixman.append(manpage_postprocess(Dir('.').path))

    # Make sure that the input XML files are File/Dir nodes.
    if isinstance(manpage, basestring):
        manpage = File(manpage)
    if isinstance(mainfile, basestring):
        mainfile = File(mainfile)

    # Generate some canned File nodes for the input XSL stuff.
    #
    # This lets things work smoothly in a variant directory.
    aptitude_common_xsl = File('../aptitude-common.xsl')
    aptitude_html_xsl = File('../aptitude-html.xsl')
    aptitude_man_xsl = File('../aptitude-man.xsl')
    aptitude_txt_xsl = File('../aptitude-txt.xsl')

    # Generate HTML and text documentation, if it exists.
    if mainfile is not None:
        # Note: automatically assume this depends on the manpage.  It
        # would be nice to have an XML/Docbook scanner that could find
        # external entity references, but this is probably easier.

        # Build the full HTML documentation:
        html = env.Docbook(Dir(output_html),
                           mainfile,
                           target_is_directory = True,
                           stylesheet = aptitude_html_xsl)
        if images is not None:
            in_svgs = env.Glob("%s/*.svg" % images)
            out_svg_pngs = []
            for in_svg in in_svgs:
                out_svg_png = '%s.png' % str(in_svg)[:-4]
                out_svg_pngs.append(env.Rsvg(out_svg_png, in_svg))
            in_images = env.Glob("%s/*.png" % images) + out_svg_pngs
            # Note that the image directory is hardcoded to "images";
            # this could be a parameter instead.
            copy_images = env.Command(os.path.join(output_html, 'images'),
                                      in_images,
                                      [ Delete('$TARGET') ] +
                                      [ Copy('$TARGET', x) for x in in_images ])
        # Note the use of InstallAs to rename the directory!
        env.InstallAs('$PKGDOCDIR/html/%s' % lang, html)
        env.Alias('doc-html', html)
        outputs.append(html)


        # Build the text documentation:
        html_onepage = env.Docbook(File(os.path.join(temp_txt, 'index.html')),
                                   mainfile,
                                   stylesheet = aptitude_txt_xsl)

        text = env.Html2Text(output_txt, html_onepage)
        env.Install('$PKGDATADIR', text)
        env.Alias('doc-text', text)
        outputs.append(text)


        # Build the manpage from the main documentation file:
        manpage_out = env.Docbook(File(os.path.join(temp_man, 'aptitude.8')),
                                  mainfile,
                                  stylesheet = aptitude_man_xsl)

        man = env.Command(output_man, manpage_out,
                          [Copy('$TARGET', '$SOURCE')] + fixman)
        env.Install('$MANDIR/%s/man8' % lang, man)
        env.Alias('doc-man', man)
        outputs.append(man)
    else:
        # Generate a manpage, if one exists.
        if manpage is not None:
            manpage_out = env.Docbook(File(os.path.join(temp_man, 'aptitude.8')),
                                      manpage,
                                      stylesheet = aptitude_man_xsl)

            man = env.Command(output_man, manpage_out,
                              [Copy('$TARGET', '$SOURCE')] + fixman)
            env.Install('$MANDIR/%s/man8' % lang, man)
            env.Alias('doc-man', man)
            outputs.append(man)

    return outputs


def AptitudePo4aDocs(env,
                     lang,
                     main_percent_translated = 75,
                     manpage_percent_translated = 75,
                     master_charset = 'utf-8',
                     add = None,
                     **kwargs):
    '''Recipe for creating documentation for aptitude by
po4a-translating the master documentation.

If add is None, an addendum is used if and only if the file
"doc/po4a/add_$LANG/addendum.1.$LANG" exists.  If add is False, an
addendum is never used.  Otherwise, the file or files named by add are
used as addenda.

percent_translated and manpage_percent_translated are used to check
that the corresponding documentation is as translated as we expect.
If one of these values is None, the corresponding file will not be
generated or compiled.

Other keyword args are interpreted as for AptitudeStandaloneDocs.

The corresponding pofiles are automatically added to the list of files
to distribute.'''

    if add is None:
        # Note that I use Dir('.').abspath in case we're in a variant
        # directory.
        testdir = os.path.join(Dir('.').abspath,
                               '../po4a/add_%s/addendum.1.%s'
                               % (lang, lang))
        if os.path.exists(testdir):
            add = File('../po4a/add_%s/addendum.1.%s' % (lang, lang))

    if add is False:
        add = None

    pofile = File('../po4a/po/%s.po' % lang)

    if main_percent_translated is None:
        translated_mainfile = None
    else:
        translated_mainfile = env.Po4A(File('aptitude.xml'),
                                       File('../en/aptitude.xml'),
                                       pofile,
                                       addendum = add,
                                       percent_translated = main_percent_translated,
                                       master_charset = master_charset,
                                       format = 'docbook')

    if manpage_percent_translated is None:
        translated_manpage = None
    else:
        translated_manpage = env.Po4A(File('manpage.xml'),
                                      File('../en/manpage.xml'),
                                      pofile,
                                      percent_translated = manpage_percent_translated,
                                      master_charset = master_charset,
                                      format = 'docbook')

    result = env.AptitudeStandaloneDocs(lang,
                                        mainfile = translated_mainfile,
                                        manpage = translated_manpage,
                                        dist_xmls = False,
                                        **kwargs)

    # Make sure the necessary files end up in the distributed archive.
    env.Dist(pofile)
    if add is not None:
        env.Dist(add)

    # The output will be installed by rules in StandaloneDocs.

    return result

