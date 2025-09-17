<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:fo="http://www.w3.org/1999/XSL/Format"
                xmlns:mml="http://www.w3.org/1998/Math/MathML"
                exclude-result-prefixes="mml"
                version='1.0'>

<!-- ********************************************************************
     $Id: math.xsl 7503 2007-10-08 01:57:30Z xmldoc $
     ********************************************************************

     This file is part of the XSL DocBook Stylesheet distribution.
     See ../README or http://docbook.sf.net/release/xsl/current/ for
     copyright and other information.

     ******************************************************************** -->

<xsl:template match="inlineequation">
  <xsl:choose>
    <xsl:when test="$passivetex.extensions != 0 and $tex.math.in.alt != ''">
      <xsl:apply-templates select="alt[@role='tex'] | inlinemediaobject/textobject[@role='tex']">
        <xsl:with-param name="output.delims">
          <xsl:call-template name="tex.math.output.delims"/>
        </xsl:with-param>
      </xsl:apply-templates>
    </xsl:when>
    <xsl:otherwise>
      <xsl:apply-templates/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="alt">
</xsl:template>

<xsl:template match="mathphrase">
  <fo:inline>
    <xsl:apply-templates/>
  </fo:inline>
</xsl:template>

<!-- "Support" for MathML -->

<xsl:template match="mml:math" xmlns:mml="http://www.w3.org/1998/Math/MathML">
  <xsl:choose>
    <!-- * If user is using passivetex, we don't wrap the output in -->
    <!-- * fo:instream-foreign-object (which passivetex doesn't support). -->
    <xsl:when test="not($passivetex.extensions = 0)">
      <xsl:copy>
        <xsl:copy-of select="@*"/>
        <xsl:apply-templates/>
      </xsl:copy>
    </xsl:when>
    <xsl:otherwise>
      <fo:instream-foreign-object>
        <xsl:copy>
          <xsl:copy-of select="@*"/>
          <xsl:apply-templates/>
        </xsl:copy>
      </fo:instream-foreign-object>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="mml:*" xmlns:mml="http://www.w3.org/1998/Math/MathML">
  <xsl:copy>
    <xsl:copy-of select="@*"/>
    <xsl:apply-templates/>
  </xsl:copy>
</xsl:template>

<xsl:template match="equation/graphic | informalequation/graphic">
  <xsl:if test="$passivetex.extensions = 0 or $tex.math.in.alt = ''">
    <fo:block>
      <xsl:call-template name="process.image"/>
    </fo:block>
  </xsl:if>
</xsl:template>

<xsl:template match="inlineequation/alt[@role='tex'] | 
                     inlineequation/inlinemediaobject/textobject[@role='tex']" priority="1">
  <xsl:param name="output.delims" select="1"/>
  <xsl:if test="$passivetex.extensions != 0 and $tex.math.in.alt != ''">
    <xsl:processing-instruction name="xmltex">
      <xsl:if test="$output.delims != 0">
        <xsl:text>$</xsl:text>
      </xsl:if>
      <xsl:value-of select="."/>
      <xsl:if test="$output.delims != 0">
        <xsl:text>$</xsl:text>
      </xsl:if>
    </xsl:processing-instruction>
  </xsl:if>
</xsl:template>

<xsl:template match="equation/alt[@role='tex'] | informalequation/alt[@role='tex'] |
                     equation/mediaobject/textobject[@role='tex'] |
                     informalequation/mediaobject/textobject[@role='tex']" priority="1">
  <xsl:variable name="output.delims">
    <xsl:call-template name="tex.math.output.delims"/>
  </xsl:variable>
  <xsl:if test="$passivetex.extensions != 0 and $tex.math.in.alt != ''">
    <xsl:processing-instruction name="xmltex">
      <xsl:if test="$output.delims != 0">
        <xsl:text>$$</xsl:text>
      </xsl:if>
      <xsl:value-of select="."/>
      <xsl:if test="$output.delims != 0">
        <xsl:text>$$</xsl:text>
      </xsl:if>
    </xsl:processing-instruction>
  </xsl:if>
</xsl:template>

<xsl:template match="alt[@role='tex']">
  <xsl:if test="$passivetex.extensions != 0 and $tex.math.in.alt != ''">
    <xsl:message>
      Your equation is misplaced. It should be in inlineequation, equation or informalequation.
    </xsl:message>
  </xsl:if>
</xsl:template>

<xsl:template name="tex.math.output.delims">
  <xsl:variable name="pi.delims">
    <xsl:call-template name="pi-attribute">
      <xsl:with-param name="pis" select=".//processing-instruction('dbtex')"/>
      <xsl:with-param name="attribute" select="'delims'"/>
    </xsl:call-template>
  </xsl:variable>
  <xsl:variable name="result">
    <xsl:choose>
      <xsl:when test="$pi.delims = 'no'">0</xsl:when>
      <xsl:when test="$pi.delims = '' and $tex.math.delims = 0">0</xsl:when>
      <xsl:otherwise>1</xsl:otherwise>
    </xsl:choose>
  </xsl:variable>
  <xsl:value-of select="$result"/>
</xsl:template>

</xsl:stylesheet>
