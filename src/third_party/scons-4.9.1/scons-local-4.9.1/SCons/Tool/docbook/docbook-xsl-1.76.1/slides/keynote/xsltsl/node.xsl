<?xml version="1.0"?>

<xsl:stylesheet version="1.0"
	xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
	xmlns:node="http://xsltsl.org/node"
	extension-element-prefixes="doc node">

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
	$Id: node.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
	<surname>Ball</surname>
	<firstname>Steve</firstname>
      </author>
      <copyright>
	<year>2001</year>
	<holder>Steve Ball</holder>
      </copyright>
    </referenceinfo>

    <title>Node Templates</title>

    <partintro>
      <section>
	<title>Introduction</title>

	<para>This stylesheet module provides functions for reporting on or manipulating nodes and nodesets.</para>

      </section>
    </partintro>

  </doc:reference>

  <doc:template name="node:xpath" xmlns="">
    <refpurpose>Returns an XPath location path</refpurpose>

    <refdescription>
      <para>This template returns an XPath location path that uniquely identifies the given node within the document.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>node</term>
	  <listitem>
	    <para>The node to create an XPath for.  If this parameter is given as a nodeset, then the first node in the nodeset is used.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns an XPath location path as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="node:xpath">
    <xsl:param name="node" select="."/>

    <xsl:choose>

      <xsl:when test="$node">

        <xsl:for-each select="$node[1]/ancestor-or-self::*">
          <xsl:text/>/<xsl:value-of select="name()"/>
          <xsl:text/>[<xsl:value-of select="count(preceding-sibling::*[name() = name(current())]) + 1"/>]<xsl:text/>
        </xsl:for-each>

        <xsl:choose>

          <xsl:when test="$node[1]/self::comment()">
            <xsl:text>/comment()</xsl:text>
            <xsl:text/>[<xsl:value-of select="count($node[1]/preceding-sibling::comment()) + 1" />]<xsl:text/>
          </xsl:when>

          <xsl:when test="$node[1]/self::processing-instruction()">
            <xsl:text>/processing-instruction()</xsl:text>
            <xsl:text/>[<xsl:value-of select="count($node[1]/preceding-sibling::processing-instruction()) + 1" />]<xsl:text/>
          </xsl:when>

          <xsl:when test="$node[1]/self::text()">
            <xsl:text>/text()</xsl:text>
            <xsl:text/>[<xsl:value-of select="count($node[1]/preceding-sibling::text()) + 1" />]<xsl:text/>
          </xsl:when>

          <xsl:when test="not($node[1]/..)">
            <xsl:text>/</xsl:text>
          </xsl:when>

          <xsl:when test="count($node[1]/../namespace::* | $node[1]) = count($node[1]/../namespace::*)">
            <xsl:text/>/namespace::<xsl:value-of select="name($node[1])" />
          </xsl:when>

          <xsl:when test="count($node[1]/../@* | $node[1]) = count($node[1]/../@*)">
            <xsl:text/>/@<xsl:value-of select="name($node[1])" />
          </xsl:when>

        </xsl:choose>      
      </xsl:when>

      <xsl:otherwise>
        <xsl:text>/..</xsl:text>
      </xsl:otherwise>

    </xsl:choose>

  </xsl:template>

  <doc:template name="node:type" xmlns="">
    <refpurpose>Return node type</refpurpose>

    <refdescription>
      <para>Returns the type of a node as a string.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>node</term>
	  <listitem>
	    <para>The node to get the type for.  If this parameter is given as a nodeset, then the first node in the nodeset is used.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns node type as a string.  Values returned are:</para>
      <variablelist>
	<varlistentry>
	  <term>Element</term>
	  <listitem>
	    <para><literal>element</literal></para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>Text Node</term>
	  <listitem>
	    <para><literal>text</literal></para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>Comment</term>
	  <listitem>
	    <para><literal>comment</literal></para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>Processing Instruction</term>
	  <listitem>
	    <para><literal>processing instruction</literal></para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refreturn>
  </doc:template>

  <xsl:template name="node:type">
    <xsl:param name="node" select="."/>

    <xsl:choose>
      <xsl:when test="not($node)"/>
      <xsl:when test="$node[1]/self::*">
	<xsl:text>element</xsl:text>
      </xsl:when>
      <xsl:when test="$node[1]/self::text()">
	<xsl:text>text</xsl:text>
      </xsl:when>
      <xsl:when test="$node[1]/self::comment()">
	<xsl:text>comment</xsl:text>
      </xsl:when>
      <xsl:when test="$node[1]/self::processing-instruction()">
	<xsl:text>processing instruction</xsl:text>
      </xsl:when>
      <xsl:when test="not($node[1]/parent::*)">
        <xsl:text>root</xsl:text>
      </xsl:when>
      <xsl:when test="count($node[1] | $node[1]/../namespace::*) = count($node[1]/../namespace::*)">
        <xsl:text>namespace</xsl:text>
      </xsl:when>
      <xsl:when test="count($node[1] | $node[1]/../@*) = count($node[1]/../@*)">
        <xsl:text>attribute</xsl:text>
      </xsl:when>
    </xsl:choose>
  </xsl:template>

  <doc:template name="node:copy" xmlns="">
    <refpurpose>Copy Nodes</refpurpose>

    <refdescription>
      <para>Makes a copy of the given nodes, including attributes and descendants.</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>nodes</term>
	  <listitem>
	    <para>The nodes to copy.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the copied nodes as a result tree fragment.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='node:copy'>
    <xsl:param name='nodes' select='.'/>

    <xsl:for-each select='$nodes'>
      <xsl:copy>
        <xsl:for-each select='@*'>
          <xsl:copy/>
        </xsl:for-each>

        <xsl:for-each select='node()'>
          <xsl:call-template name='node:copy'/>
        </xsl:for-each>
      </xsl:copy>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>

