<?xml version="1.0"?>

<xsl:stylesheet
  version="1.0"
  extension-element-prefixes="doc"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  xmlns:eg="http://xsltsl.org/example"
>

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: example.xsl 3991 2004-11-10 06:51:55Z balls $
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

    <title>Example Stylesheet</title>

    <partintro>
      <section>
        <title>Introduction</title>

        <para>This module provides a template for adding stylesheet modules to the XSLT Standard Library.</para>
        <para>To add a new module to the library, follow these easy steps:</para>
        <orderedlist>
          <listitem>
            <para>Copy this file and replace its contents with the new module templates and documentation.</para>
          </listitem>
          <listitem>
            <para>Copy the corresponding test file in the <filename>test</filename> directory.  Replace its contents with tests for the new module.</para>
          </listitem>
          <listitem>
            <para>Add an include element in the <filename>stdlib.xsl</filename> stylesheet.</para>
          </listitem>
          <listitem>
            <para>Add an entry in the <filename>test/test.xml</filename> file.</para>
          </listitem>
          <listitem>
            <para>Add entries in the <filename>test/test.xsl</filename> stylesheet.</para>
          </listitem>
          <listitem>
            <para>Add an entry in the <filename>doc/build.xml</filename> file.</para>
          </listitem>
        </orderedlist>

        <para>The <filename>example.xsl</filename> stylesheet provides a more extensive example.</para>

      </section>
    </partintro>

  </doc:reference>

  <doc:template name="eg:example" xmlns="">
    <refpurpose>Template Example</refpurpose>

    <refdescription>
      <para>Provides a template for writing templates.  Replace this paragraph with a description of your template</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>text</term>
          <listitem>
            <para>The example string</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns nothing.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="eg:example">
    <xsl:param name="text"/>
  </xsl:template>

</xsl:stylesheet>

