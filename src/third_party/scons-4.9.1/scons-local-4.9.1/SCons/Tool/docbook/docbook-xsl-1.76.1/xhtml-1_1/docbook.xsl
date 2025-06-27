<?xml version="1.0" encoding="ASCII"?>
<!--This file was created automatically by html2xhtml-->
<!--from the HTML stylesheets.-->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:ng="http://docbook.org/docbook-ng" xmlns:db="http://docbook.org/ns/docbook" xmlns:exsl="http://exslt.org/common" xmlns:exslt="http://exslt.org/common" xmlns="http://www.w3.org/1999/xhtml" exclude-result-prefixes="db ng exsl exslt" version="1.0">

<xsl:output method="xml" encoding="UTF-8" indent="no" doctype-public="-//W3C//DTD XHTML 1.1//EN" doctype-system="http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"/>

<!-- ********************************************************************
     $Id: docbook.xsl 8783 2010-07-28 10:59:39Z mzjn $
     ********************************************************************

     This file is part of the XSL DocBook Stylesheet distribution.
     See ../README or http://docbook.sf.net/release/xsl/current/ for
     copyright and other information.

     ******************************************************************** -->

<!-- ==================================================================== -->

<xsl:include href="../VERSION"/>
<xsl:include href="param.xsl"/>
<xsl:include href="../lib/lib.xsl"/>
<xsl:include href="../common/l10n.xsl"/>
<xsl:include href="../common/common.xsl"/>
<xsl:include href="../common/utility.xsl"/>
<xsl:include href="../common/labels.xsl"/>
<xsl:include href="../common/titles.xsl"/>
<xsl:include href="../common/subtitles.xsl"/>
<xsl:include href="../common/gentext.xsl"/>
<xsl:include href="../common/targets.xsl"/>
<xsl:include href="../common/olink.xsl"/>
<xsl:include href="../common/pi.xsl"/>
<xsl:include href="autotoc.xsl"/>
<xsl:include href="autoidx.xsl"/>
<xsl:include href="lists.xsl"/>
<xsl:include href="callout.xsl"/>
<xsl:include href="verbatim.xsl"/>
<xsl:include href="graphics.xsl"/>
<xsl:include href="xref.xsl"/>
<xsl:include href="formal.xsl"/>
<xsl:include href="table.xsl"/>
<xsl:include href="htmltbl.xsl"/>
<xsl:include href="sections.xsl"/>
<xsl:include href="inline.xsl"/>
<xsl:include href="footnote.xsl"/>
<xsl:include href="html.xsl"/>
<xsl:include href="info.xsl"/>
<xsl:include href="keywords.xsl"/>
<xsl:include href="division.xsl"/>
<xsl:include href="toc.xsl"/>
<xsl:include href="index.xsl"/>
<xsl:include href="refentry.xsl"/>
<xsl:include href="math.xsl"/>
<xsl:include href="admon.xsl"/>
<xsl:include href="component.xsl"/>
<xsl:include href="biblio.xsl"/>
<xsl:include href="biblio-iso690.xsl"/>
<xsl:include href="glossary.xsl"/>
<xsl:include href="block.xsl"/>
<xsl:include href="task.xsl"/>
<xsl:include href="qandaset.xsl"/>
<xsl:include href="synop.xsl"/>
<xsl:include href="titlepage.xsl"/>
<xsl:include href="titlepage.templates.xsl"/>
<xsl:include href="pi.xsl"/>
<xsl:include href="ebnf.xsl"/>
<xsl:include href="chunker.xsl"/>
<xsl:include href="html-rtf.xsl"/>
<xsl:include href="annotations.xsl"/>
<xsl:include href="../common/stripns.xsl"/>

<xsl:param name="stylesheet.result.type" select="'xhtml'"/>
<xsl:param name="htmlhelp.output" select="0"/>

<!-- ==================================================================== -->

<xsl:key name="id" match="*" use="@id|@xml:id"/>
<xsl:key name="gid" match="*" use="generate-id()"/>

<!-- ==================================================================== -->

<xsl:template match="*">
  <xsl:message>
    <xsl:text>Element </xsl:text>
    <xsl:value-of select="local-name(.)"/>
    <xsl:text> in namespace '</xsl:text>
    <xsl:value-of select="namespace-uri(.)"/>
    <xsl:text>' encountered</xsl:text>
    <xsl:if test="parent::*">
      <xsl:text> in </xsl:text>
      <xsl:value-of select="name(parent::*)"/>
    </xsl:if>
    <xsl:text>, but no template matches.</xsl:text>
  </xsl:message>

  <span style="color: red">
    <xsl:text>&lt;</xsl:text>
    <xsl:value-of select="name(.)"/>
    <xsl:text>&gt;</xsl:text>
    <xsl:apply-templates/>
    <xsl:text>&lt;/</xsl:text>
    <xsl:value-of select="name(.)"/>
    <xsl:text>&gt;</xsl:text>
  </span>
</xsl:template>

<xsl:template match="text()">
  <xsl:value-of select="."/>
</xsl:template>

<xsl:template name="body.attributes">
<!-- no apply-templates; make it empty -->
</xsl:template>

<xsl:template name="head.content">
  <xsl:param name="node" select="."/>
  <xsl:param name="title">
    <xsl:apply-templates select="$node" mode="object.title.markup.textonly"/>
  </xsl:param>

  <title>
    <xsl:copy-of select="$title"/>
  </title>

  <xsl:if test="$html.base != ''">
    <base href="{$html.base}"/>
  </xsl:if>

  <!-- Insert links to CSS files or insert literal style elements -->
  <xsl:call-template name="generate.css"/>

  <xsl:if test="$html.stylesheet != ''">
    <xsl:call-template name="output.html.stylesheets">
      <xsl:with-param name="stylesheets" select="normalize-space($html.stylesheet)"/>
    </xsl:call-template>
  </xsl:if>

  <xsl:if test="$link.mailto.url != ''">
    <link rev="made" href="{$link.mailto.url}"/>
  </xsl:if>

  <meta name="generator" content="DocBook {$DistroTitle} V{$VERSION}"/>

  <xsl:if test="$generate.meta.abstract != 0">
    <xsl:variable name="info" select="(articleinfo                                       |bookinfo                                       |prefaceinfo                                       |chapterinfo                                       |appendixinfo                                       |sectioninfo                                       |sect1info                                       |sect2info                                       |sect3info                                       |sect4info                                       |sect5info                                       |referenceinfo                                       |refentryinfo                                       |partinfo                                       |info                                       |docinfo)[1]"/>
    <xsl:if test="$info and $info/abstract">
      <meta name="description">
        <xsl:attribute name="content">
          <xsl:for-each select="$info/abstract[1]/*">
            <xsl:value-of select="normalize-space(.)"/>
            <xsl:if test="position() &lt; last()">
              <xsl:text> </xsl:text>
            </xsl:if>
          </xsl:for-each>
        </xsl:attribute>
      </meta>
    </xsl:if>
  </xsl:if>

  <xsl:if test="($draft.mode = 'yes' or                 ($draft.mode = 'maybe' and                 ancestor-or-self::*[@status][1]/@status = 'draft'))                 and $draft.watermark.image != ''">
    <style type="text/css"><xsl:text>
body { background-image: url('</xsl:text>
<xsl:value-of select="$draft.watermark.image"/><xsl:text>');
       background-repeat: no-repeat;
       background-position: top left;
       /* The following properties make the watermark "fixed" on the page. */
       /* I think that's just a bit too distracting for the reader... */
       /* background-attachment: fixed; */
       /* background-position: center center; */
     }</xsl:text>
    </style>
  </xsl:if>
  <xsl:apply-templates select="." mode="head.keywords.content"/>
</xsl:template>

<xsl:template name="output.html.stylesheets">
  <xsl:param name="stylesheets" select="''"/>

  <xsl:choose>
    <xsl:when test="contains($stylesheets, ' ')">
      <xsl:variable name="css.filename" select="substring-before($stylesheets, ' ')"/>

      <xsl:call-template name="make.css.link">
        <xsl:with-param name="css.filename" select="$css.filename"/>
      </xsl:call-template>

      <xsl:call-template name="output.html.stylesheets">
        <xsl:with-param name="stylesheets" select="substring-after($stylesheets, ' ')"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:when test="$stylesheets != ''">
      <xsl:call-template name="make.css.link">
        <xsl:with-param name="css.filename" select="$stylesheets"/>
      </xsl:call-template>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<!-- ============================================================ -->

<xsl:template match="*" mode="head.keywords.content">
  <xsl:apply-templates select="chapterinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="appendixinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="prefaceinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="bookinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="setinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="articleinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="artheader/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sect1info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sect2info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sect3info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sect4info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sect5info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="sectioninfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="refsect1info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="refsect2info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="refsect3info/keywordset" mode="html.header"/>
  <xsl:apply-templates select="bibliographyinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="glossaryinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="indexinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="refentryinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="partinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="referenceinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="docinfo/keywordset" mode="html.header"/>
  <xsl:apply-templates select="info/keywordset" mode="html.header"/>

  <xsl:if test="$inherit.keywords != 0                 and parent::*">
    <xsl:apply-templates select="parent::*" mode="head.keywords.content"/>
  </xsl:if>
</xsl:template>

<!-- ============================================================ -->

<xsl:template name="system.head.content">
  <xsl:param name="node" select="."/>

  <!-- FIXME: When chunking, only the annotations actually used
              in this chunk should be referenced. I don't think it
              does any harm to reference them all, but it adds
              unnecessary bloat to each chunk. -->
  <xsl:if test="$annotation.support != 0 and //annotation">
    <xsl:call-template name="add.annotation.links"/>
    <script type="text/javascript">
      <xsl:text>
// Create PopupWindow objects</xsl:text>
      <xsl:for-each select="//annotation">
        <xsl:text>
var popup_</xsl:text>
        <xsl:value-of select="generate-id(.)"/>
        <xsl:text> = new PopupWindow("popup-</xsl:text>
        <xsl:value-of select="generate-id(.)"/>
        <xsl:text>");
</xsl:text>
        <xsl:text>popup_</xsl:text>
        <xsl:value-of select="generate-id(.)"/>
        <xsl:text>.offsetY = 15;
</xsl:text>
        <xsl:text>popup_</xsl:text>
        <xsl:value-of select="generate-id(.)"/>
        <xsl:text>.autoHide();
</xsl:text>
      </xsl:for-each>
    </script>

    <style type="text/css">
      <xsl:value-of select="$annotation.css"/>
    </style>
  </xsl:if>

  <!-- system.head.content is like user.head.content, except that
       it is called before head.content. This is important because it
       means, for example, that <style> elements output by system.head.content
       have a lower CSS precedence than the users stylesheet. -->
</xsl:template>

<!-- ============================================================ -->

<xsl:template name="user.preroot">
  <!-- Pre-root output, can be used to output comments and PIs. -->
  <!-- This must not output any element content! -->
</xsl:template>

<xsl:template name="user.head.content">
  <xsl:param name="node" select="."/>
</xsl:template>

<xsl:template name="user.header.navigation">
  <xsl:param name="node" select="."/>
</xsl:template>

<xsl:template name="user.header.content">
  <xsl:param name="node" select="."/>
</xsl:template>

<xsl:template name="user.footer.content">
  <xsl:param name="node" select="."/>
</xsl:template>

<xsl:template name="user.footer.navigation">
  <xsl:param name="node" select="."/>
</xsl:template>

<xsl:template match="/">
  <!-- * Get a title for current doc so that we let the user -->
  <!-- * know what document we are processing at this point. -->
  <xsl:variable name="doc.title">
    <xsl:call-template name="get.doc.title"/>
  </xsl:variable>
  <xsl:choose>
    <!-- Hack! If someone hands us a DocBook V5.x or DocBook NG document,
         toss the namespace and continue.  Use the docbook5 namespaced
         stylesheets for DocBook5 if you don't want to use this feature.-->
    <!-- include extra test for Xalan quirk -->
    <xsl:when test="$exsl.node.set.available != 0                     and (*/self::ng:* or */self::db:*)">
      <xsl:call-template name="log.message">
        <xsl:with-param name="level">Note</xsl:with-param>
        <xsl:with-param name="source" select="$doc.title"/>
        <xsl:with-param name="context-desc">
          <xsl:text>namesp. cut</xsl:text>
        </xsl:with-param>
        <xsl:with-param name="message">
          <xsl:text>stripped namespace before processing</xsl:text>
        </xsl:with-param>
      </xsl:call-template>
      <xsl:variable name="nons">
        <xsl:apply-templates mode="stripNS"/>
      </xsl:variable>
      <!--
      <xsl:message>Saving stripped document.</xsl:message>
      <xsl:call-template name="write.chunk">
        <xsl:with-param name="filename" select="'/tmp/stripped.xml'"/>
        <xsl:with-param name="method" select="'xml'"/>
        <xsl:with-param name="content">
          <xsl:copy-of select="exsl:node-set($nons)"/>
        </xsl:with-param>
      </xsl:call-template>
      -->
      <xsl:call-template name="log.message">
        <xsl:with-param name="level">Note</xsl:with-param>
        <xsl:with-param name="source" select="$doc.title"/>
        <xsl:with-param name="context-desc">
          <xsl:text>namesp. cut</xsl:text>
        </xsl:with-param>
        <xsl:with-param name="message">
          <xsl:text>processing stripped document</xsl:text>
        </xsl:with-param>
      </xsl:call-template>
      <xsl:apply-templates select="exsl:node-set($nons)"/>
    </xsl:when>
    <!-- Can't process unless namespace removed -->
    <xsl:when test="*/self::ng:* or */self::db:*">
      <xsl:message terminate="yes">
        <xsl:text>Unable to strip the namespace from DB5 document,</xsl:text>
        <xsl:text> cannot proceed.</xsl:text>
      </xsl:message>
    </xsl:when>
    <xsl:otherwise>
      <xsl:choose>
        <xsl:when test="$rootid != ''">
          <xsl:choose>
            <xsl:when test="count(key('id',$rootid)) = 0">
              <xsl:message terminate="yes">
                <xsl:text>ID '</xsl:text>
                <xsl:value-of select="$rootid"/>
                <xsl:text>' not found in document.</xsl:text>
              </xsl:message>
            </xsl:when>
            <xsl:otherwise>
              <xsl:if test="$collect.xref.targets = 'yes' or                             $collect.xref.targets = 'only'">
                <xsl:apply-templates select="key('id', $rootid)" mode="collect.targets"/>
              </xsl:if>
              <xsl:if test="$collect.xref.targets != 'only'">
                <xsl:apply-templates select="key('id',$rootid)" mode="process.root"/>
                <xsl:if test="$tex.math.in.alt != ''">
                  <xsl:apply-templates select="key('id',$rootid)" mode="collect.tex.math"/>
                </xsl:if>
              </xsl:if>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:when>
        <xsl:otherwise>
          <xsl:if test="$collect.xref.targets = 'yes' or                         $collect.xref.targets = 'only'">
            <xsl:apply-templates select="/" mode="collect.targets"/>
          </xsl:if>
          <xsl:if test="$collect.xref.targets != 'only'">
            <xsl:apply-templates select="/" mode="process.root"/>
            <xsl:if test="$tex.math.in.alt != ''">
              <xsl:apply-templates select="/" mode="collect.tex.math"/>
            </xsl:if>
          </xsl:if>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="*" mode="process.root">
  <xsl:variable name="doc" select="self::*"/>

  <xsl:call-template name="user.preroot"/>
  <xsl:call-template name="root.messages"/>

  <html>
    <head>
      <xsl:call-template name="system.head.content">
        <xsl:with-param name="node" select="$doc"/>
      </xsl:call-template>
      <xsl:call-template name="head.content">
        <xsl:with-param name="node" select="$doc"/>
      </xsl:call-template>
      <xsl:call-template name="user.head.content">
        <xsl:with-param name="node" select="$doc"/>
      </xsl:call-template>
    </head>
    <body>
      <xsl:call-template name="body.attributes"/>
      <xsl:call-template name="user.header.content">
        <xsl:with-param name="node" select="$doc"/>
      </xsl:call-template>
      <xsl:apply-templates select="."/>
      <xsl:call-template name="user.footer.content">
        <xsl:with-param name="node" select="$doc"/>
      </xsl:call-template>
    </body>
  </html>
  <xsl:value-of select="$html.append"/>
  
  <!-- Generate any css files only once, not once per chunk -->
  <xsl:call-template name="generate.css.files"/>
</xsl:template>

<xsl:template name="root.messages">
  <!-- redefine this any way you'd like to output messages -->
  <!-- DO NOT OUTPUT ANYTHING FROM THIS TEMPLATE -->
</xsl:template>

<!-- ==================================================================== -->

<xsl:template name="chunk">
  <xsl:param name="node" select="."/>

  <!-- The default is that we are not chunking... -->
  <xsl:text>0</xsl:text>
</xsl:template>

</xsl:stylesheet>
