<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<!--
SPDX-License-Identifier: MIT

Copyright The SCons Foundation

xmldepend.xsl - Find files that an XML document refers to/depends on.

This stylesheet can be used to build Makefile dependency lists.

Dependencies are defined as:
- Files named in <xi:xinclude> elements. These must be traversed to
  find any subdependencies, unless parse attribute is "text".
- Files named by the fileref attribute of any other element.

Who-to-blame:
Paul DuBois
paul@kitebird.com
2005-08-16

Change history:
2005-08-16
- Version 1.00.

TODO:
- add params for whether to produce debugging output, etc.
-->

<xsl:output method="text" indent="no"/>

<!-- BEGIN PARAMETERS -->

<xsl:param name="xmldepend.terminator" select="'&#10;'"/>

<!-- END PARAMETERS -->

<!-- BEGIN UTILITY TEMPLATES -->

<!--
  Given a pathname, return the basename (part after last '/'):
  - If path contains no '/' separators, return entire value
  - If path contains '/' separator, recurse using part after first one
-->

<xsl:template name="path-basename">
  <xsl:param name="path"/>
  <xsl:choose>
    <xsl:when test="not(contains($path,'/'))">
      <xsl:value-of select="$path"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:call-template name="path-basename">
        <xsl:with-param name="path" select="substring-after($path,'/')"/>
      </xsl:call-template>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!--
  Given a pathname, return the dirname (part up through last '/'):
  - If path contains no '/' separators, return empty string
  - If path contains '/' separator, return part up through last one
    (which is the same as the part before the basename)
-->

<xsl:template name="path-dirname">
  <xsl:param name="path"/>
  <xsl:choose>
    <xsl:when test="not(contains($path,'/'))">
      <!-- return nothing -->
    </xsl:when>
    <xsl:otherwise>
      <xsl:variable name="basename">
        <xsl:call-template name="path-basename">
          <xsl:with-param name="path" select="$path"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:value-of select="substring(           $path,1,string-length($path) - string-length($basename)       )"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- END UTILITY TEMPLATES -->

<!--
  Find XInclude directives, spit out the href value that names
  the included file, and process the file recursively if it's
  an XML file.
-->

<xsl:template match="xi:include" xmlns:xi="http://www.w3.org/2001/XInclude">
  <xsl:param name="curdir"/>

  <xsl:variable name="href-dir">
    <xsl:call-template name="path-dirname">
      <xsl:with-param name="path" select="@href"/>
    </xsl:call-template>
  </xsl:variable>

  <!--
    Display pathname of included file.  It should be prefixed by
    the directory of the referring file unless the included file
    is an absolute pathname.
  -->

  <xsl:if test="not(starts-with(@href,'/'))">
    <xsl:value-of select="$curdir"/>
  </xsl:if>
  <xsl:value-of select="@href"/>
  <xsl:value-of select="$xmldepend.terminator"/>

  <!--
    Process included file (unless parse="text").
    Pass directory of included file while processing it
    so that relative pathnames referenced in the document
    can be resolved properly.
  -->

  <xsl:if test="not(@parse = 'text')">
    <xsl:for-each select="document(@href)">
      <xsl:apply-templates>
        <xsl:with-param name="curdir">
          <xsl:if test="not(starts-with(@href,'/'))">
            <xsl:value-of select="$curdir"/>
          </xsl:if>
          <xsl:value-of select="$href-dir"/>
        </xsl:with-param>
      </xsl:apply-templates>
    </xsl:for-each>
  </xsl:if>
</xsl:template>

<!--
  Several elements have a fileref attribute.  Spit out the file named
  by any of them.  Resolve the filename relative to the directory of
  the referencing file unless the name is an absolute pathname.
-->

<xsl:template match="*[@fileref]">
  <xsl:param name="curdir"/>

  <xsl:if test="not(starts-with(@fileref,'/'))">
    <xsl:value-of select="$curdir"/>
  </xsl:if>
  <xsl:value-of select="@fileref"/>
  <xsl:value-of select="$xmldepend.terminator"/>
</xsl:template>

<!-- Identity transform, but keep track of current document directory -->

<xsl:template match="*">
  <xsl:param name="curdir"/>
  <xsl:apply-templates select="*">
    <xsl:with-param name="curdir" select="$curdir"/>
  </xsl:apply-templates>
</xsl:template>

</xsl:stylesheet>
