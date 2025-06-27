<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  xmlns:math="http://xsltsl.org/math"
  exclude-result-prefixes="doc math">

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: math.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
        <surname>Ball</surname>
        <firstname>Steve</firstname>
      </author>
      <copyright>
        <year>2004</year>
        <year>2002</year>
        <holder>Steve Ball</holder>
      </copyright>
    </referenceinfo>

    <title>Math Module</title>

    <partintro>
      <section>
        <title>Introduction</title>

        <para>This module provides mathematical functions.</para>
      </section>
    </partintro>

  </doc:reference>

  <doc:template name="math:power" xmlns="">
    <refpurpose>Power</refpurpose>

    <refdescription>
      <para>Raises a number to a power.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>base</term>
          <listitem>
            <para>The base number.  Must be a number.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>power</term>
          <listitem>
            <para>The power to raise the number to.  Must be an integer.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns base multiplied by itself power times.  If the base or power are not numbers or if the power is fractional then an empty string is returned.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:power">
    <xsl:param name="base"/>
    <xsl:param name="power"/>

    <xsl:choose>
      <xsl:when test='$power = "0" and $base = "0"'>
        <xsl:text>1</xsl:text>
      </xsl:when>
      <xsl:when test='$power = "0" and number($base)'>
        <xsl:text>1</xsl:text>
      </xsl:when>
      <xsl:when test='$power = "0" and not(number($base))'/>
      <xsl:when test='$base = "0" and number($power)'>
        <xsl:text>0</xsl:text>
      </xsl:when>

      <xsl:when test='not(number($base)) or not(number($power))'/>

      <xsl:when test='floor(number($power)) != number($power)'/>

      <xsl:when test='number($power) &lt; 0'>
        <xsl:variable name='x'>
          <xsl:call-template name='math:power'>
            <xsl:with-param name='base' select='$base'/>
            <xsl:with-param name='power' select='-1 * $power'/>
          </xsl:call-template>
        </xsl:variable>
        <xsl:value-of select='1 div $x'/>
      </xsl:when>

      <xsl:when test='number($power) = 1'>
        <xsl:value-of select='$base'/>
      </xsl:when>

      <xsl:when test='number($power) &gt; 0'>
        <xsl:variable name='x'>
          <xsl:call-template name='math:power'>
            <xsl:with-param name='base' select='$base'/>
            <xsl:with-param name='power' select='$power - 1'/>
          </xsl:call-template>
        </xsl:variable>
        <xsl:value-of select='$base * $x'/>
      </xsl:when>
      <xsl:otherwise/>
    </xsl:choose>
  </xsl:template>

  <doc:template name="math:abs" xmlns="">
    <refpurpose>Absolute Value</refpurpose>

    <refdescription>
      <para>Absolute value of a number.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>number</term>
          <listitem>
            <para>The number.  Must be a number.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the absolute value of the number.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:abs">
    <xsl:param name="number"/>

    <xsl:choose>
      <xsl:when test='$number &lt; 0'>
        <xsl:value-of select='$number * -1'/>
      </xsl:when>
      <xsl:when test='$number >= 0'>
        <xsl:value-of select='$number'/>
      </xsl:when>
    </xsl:choose>
  </xsl:template>

  <doc:template name="math:cvt-hex-decimal" xmlns="">
    <refpurpose>Conversion</refpurpose>

    <refdescription>
      <para>Converts a hexidecimal value to a decimal value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>value</term>
          <listitem>
            <para>The hexidecimal number.  Must be a number in hexidecimal format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the value as a decimal string.  If the value is not a number then a NaN value is returned.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:cvt-hex-decimal">
    <xsl:param name="value"/>

    <xsl:choose>
      <xsl:when test='$value = ""'/>

      <xsl:when test='string-length($value) = 1'>
        <xsl:call-template name='math:cvt-hex-decimal-digit'>
          <xsl:with-param name='digit' select='$value'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:variable name='first-digit'>
          <xsl:call-template name='math:cvt-hex-decimal-digit'>
            <xsl:with-param name='digit' select='substring($value, 1, 1)'/>
          </xsl:call-template>
        </xsl:variable>
        <xsl:variable name='remainder'>
          <xsl:call-template name='math:cvt-hex-decimal'>
            <xsl:with-param name='value' select='substring($value, 2)'/>
          </xsl:call-template>
        </xsl:variable>

        <xsl:value-of select='$first-digit * 16 + $remainder'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template name='math:cvt-hex-decimal-digit'>
    <xsl:param name='digit' select='0'/>
    <xsl:choose>
      <xsl:when test='$digit &lt;= 9'>
        <xsl:value-of select='$digit'/>
      </xsl:when>
      <xsl:when test='$digit = "a" or $digit = "A"'>10</xsl:when>
      <xsl:when test='$digit = "b" or $digit = "B"'>11</xsl:when>
      <xsl:when test='$digit = "c" or $digit = "C"'>12</xsl:when>
      <xsl:when test='$digit = "d" or $digit = "D"'>13</xsl:when>
      <xsl:when test='$digit = "e" or $digit = "E"'>14</xsl:when>
      <xsl:when test='$digit = "f" or $digit = "F"'>15</xsl:when>
    </xsl:choose>
  </xsl:template>

  <doc:template name="math:cvt-decimal-hex" xmlns="">
    <refpurpose>Conversion</refpurpose>

    <refdescription>
      <para>Converts a decimal value to a hexidecimal value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>value</term>
          <listitem>
            <para>The decimal number.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the value as a hexidecimal string (lowercase).  If the value is not a number then a NaN value is returned.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:cvt-decimal-hex">
    <xsl:param name="value"/>

    <xsl:choose>
      <xsl:when test='$value = "0"'>0</xsl:when>
      <xsl:when test='not(number($value))'>NaN</xsl:when>

      <xsl:when test='$value div 16 >= 1'>
        <xsl:call-template name='math:cvt-decimal-hex'>
          <xsl:with-param name='value' select='floor($value div 16)'/>
        </xsl:call-template>
        <xsl:call-template name='math:cvt-decimal-hex'>
          <xsl:with-param name='value' select='$value mod 16'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$value = 10'>a</xsl:when>
      <xsl:when test='$value = 11'>b</xsl:when>
      <xsl:when test='$value = 12'>c</xsl:when>
      <xsl:when test='$value = 13'>d</xsl:when>
      <xsl:when test='$value = 14'>e</xsl:when>
      <xsl:when test='$value = 15'>f</xsl:when>
      <xsl:otherwise>
        <xsl:value-of select='$value'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="math:ordinal" xmlns="">
    <refpurpose>Ordinal number</refpurpose>

    <refdescription>
      <para>Gives the ordinal number of a given counting number.  For example, 1 becomes "1st".</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>number</term>
          <listitem>
            <para>An integer number.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the number with an ordinal suffix.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:ordinal">
    <xsl:param name="number"/>

    <xsl:choose>
      <xsl:when test='$number &lt; 0'/>
      <xsl:otherwise>
        <xsl:value-of select='$number'/>
        <xsl:choose>
          <xsl:when test='$number = 11 or $number = 12 or $number = 13'>th</xsl:when>
          <xsl:when test='$number mod 10 = 1'>st</xsl:when>
          <xsl:when test='$number mod 10 = 2'>nd</xsl:when>
          <xsl:when test='$number mod 10 = 3'>rd</xsl:when>
          <xsl:otherwise>th</xsl:otherwise>
        </xsl:choose>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>


  <doc:template name="math:ordinal-as-word" xmlns="">
    <refpurpose>Returns an ordinal number</refpurpose>

    <refdescription>
      <para>This template returns the ordinal number for a given counting number as a word.  For example "first" for 1.</para>
      <para>Only handles numbers less than 10000000 (ten million).</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>number</term>
	  <listitem>
	    <para>The counting number.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>conjunctive</term>
	  <listitem>
	    <para>Whether to add the word "and" to the result, for example "one hundred and first" rather than "one hundred first".  Default is "yes".</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the ordinal number as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:ordinal-as-word">
    <xsl:param name="number" select="0"/>
    <xsl:param name='conjunctive' select='"yes"'/>
    <xsl:param name='preceding' select='0'/>

    <xsl:choose>
      <xsl:when test='$preceding = 1 and $number = 0'/>
      <xsl:when test='$number = 0'>zeroth</xsl:when>

      <xsl:when test="$number &lt; 1 or $number != floor($number)"/>

      <xsl:when test='$number = 1'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>first</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 2'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>second</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 3'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>third</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 4'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fourth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 5'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fifth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 6'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>sixth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 7'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>seventh</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 8'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>eighth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 9'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>ninth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 10'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>tenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 11'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>eleventh</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 12'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>twelveth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 13'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>thirteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 14'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fourteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 15'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fifteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 16'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>sixteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 17'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>seventeenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 18'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>eighteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 19'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>nineteenth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 20'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>twentieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 30'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>thirtieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 40'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fortieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 50'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>fiftieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 60'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>sixtieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 70'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>seventieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 80'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>eightieth</xsl:text>
      </xsl:when>
      <xsl:when test='$number = 90'>
        <xsl:if test='$preceding = 1'> and </xsl:if>
        <xsl:text>ninetieth</xsl:text>
      </xsl:when>

      <xsl:when test='$number mod 1000000 = 0'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000000)'/>
        </xsl:call-template>
        <xsl:text> millionth</xsl:text>
      </xsl:when>
      <xsl:when test='$number &lt; 1000000 and $number mod 1000 = 0'>
        <xsl:if test='$preceding = 1 and $conjunctive'> and </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000)'/>
        </xsl:call-template>
        <xsl:text> thousandth</xsl:text>
      </xsl:when>
      <xsl:when test='$number &lt; 1000 and $number mod 100 = 0'>
        <xsl:if test='$preceding = 1 and $conjunctive'> and </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 100)'/>
        </xsl:call-template>
        <xsl:text> hundredth</xsl:text>
      </xsl:when>

      <xsl:when test='$number &gt; 1000000'>
        <xsl:if test='$preceding = 1'>
          <xsl:text> </xsl:text>
          <xsl:if test='$conjunctive'>and </xsl:if>
        </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000000) * 1000000'/>
        </xsl:call-template>
        <xsl:choose>
          <xsl:when
            test='(floor(floor(($number mod 1000000) + 0.1) div 100000) > 0 and $number mod 100000 > 0) or
            (floor(floor(($number mod 100000) + 0.1) div 10000) > 0 and $number mod 10000 > 0) or
            (floor(floor(($number mod 10000) + 0.1) div 1000) > 0 and $number mod 1000 > 0) or
            (floor(floor(($number mod 1000) + 0.1) div 100) > 0 and $number mod 100 > 0) or
            (floor(floor(($number mod 100) + 0.1) div 10) > 0 and $number mod 10 > 0 and $number mod 100 > 20)'>
            <xsl:text> </xsl:text>
            <xsl:call-template name='math:ordinal-as-word'>
              <xsl:with-param name='number' select='floor(($number mod 1000000) + 0.1)'/>
              <xsl:with-param name='conjunctive' select='$conjunctive'/>
              <xsl:with-param name='preceding' select='0'/>
            </xsl:call-template>
          </xsl:when>
          <xsl:otherwise>
            <xsl:call-template name='math:ordinal-as-word'>
              <xsl:with-param name='number' select='floor(($number mod 1000000) + 0.1)'/>
              <xsl:with-param name='conjunctive' select='$conjunctive'/>
              <xsl:with-param name='preceding' select='1'/>
            </xsl:call-template>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:when test='$number &gt; 1000'>
        <xsl:if test='$preceding = 1'>
          <xsl:text> </xsl:text>
          <xsl:if test='$conjunctive'>and </xsl:if>
        </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000) * 1000'/>
          <xsl:with-param name='conjunctive' select='$conjunctive'/>
        </xsl:call-template>
        <xsl:choose>
          <xsl:when test='floor(floor(($number mod 1000) + 0.1) div 100) > 0'>
            <xsl:text> </xsl:text>
            <xsl:call-template name='math:ordinal-as-word'>
              <xsl:with-param name='number' select='floor(($number mod 1000) + 0.1)'/>
              <xsl:with-param name='conjunctive' select='$conjunctive'/>
              <xsl:with-param name='preceding' select='0'/>
            </xsl:call-template>
          </xsl:when>
          <xsl:otherwise>
            <xsl:call-template name='math:ordinal-as-word'>
              <xsl:with-param name='number' select='floor(($number mod 1000) + 0.1)'/>
              <xsl:with-param name='conjunctive' select='$conjunctive'/>
              <xsl:with-param name='preceding' select='1'/>
            </xsl:call-template>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:when test='$number &gt; 100'>
        <xsl:if test='$preceding = 1'>
          <xsl:text> </xsl:text>
          <xsl:if test='$conjunctive'>and </xsl:if>
        </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 100) * 100'/>
        </xsl:call-template>
        <xsl:call-template name='math:ordinal-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 100) + 0.1)'/>
          <xsl:with-param name='conjunctive' select='$conjunctive'/>
          <xsl:with-param name='preceding' select='1'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:when test='$number &gt; 20'>
        <xsl:if test='$preceding = 1'>
          <xsl:text> </xsl:text>
          <xsl:if test='$conjunctive'>and </xsl:if>
        </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 10) * 10'/>
        </xsl:call-template>
        <xsl:text> </xsl:text>
        <xsl:call-template name='math:ordinal-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 10) + 0.1)'/>
          <xsl:with-param name='conjunctive' select='$conjunctive'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:otherwise/>
    </xsl:choose>
  </xsl:template>

  <doc:template name="math:number-as-word" xmlns="">
    <refpurpose>Returns a number as a word</refpurpose>

    <refdescription>
      <para>This template returns the word for a given integer number, for example "one" for 1.</para>
      <para>Only handles numbers less than 10000000 (ten million).</para>
    </refdescription>

    <refparameter>
      <variablelist>
	<varlistentry>
	  <term>number</term>
	  <listitem>
	    <para>The counting number.</para>
	  </listitem>
	</varlistentry>
	<varlistentry>
	  <term>conjunctive</term>
	  <listitem>
	    <para>Adds the word "and" where appropriate, for example.</para>
	  </listitem>
	</varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the number as a string.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="math:number-as-word">
    <xsl:param name="number" select="0"/>
    <xsl:param name='conjunctive' select='true()'/>

    <xsl:choose>

      <xsl:when test='$number = 0'>zero</xsl:when>

      <xsl:when test='$number &lt; 0'>
        <xsl:text>minus </xsl:text>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='-1 * $number'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:when test="$number != floor($number)"/>

      <xsl:when test='$number mod 1000000 = 0'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000000)'/>
        </xsl:call-template>
        <xsl:text> million</xsl:text>
      </xsl:when>
      <xsl:when test='$number &gt;= 1000000'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000000)'/>
        </xsl:call-template>
        <xsl:text> million </xsl:text>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 1000000) + 0.1)'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$number mod 1000 = 0'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000)'/>
        </xsl:call-template>
        <xsl:text> thousand</xsl:text>
      </xsl:when>
      <xsl:when test='$number &gt;= 1000'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 1000)'/>
        </xsl:call-template>
        <xsl:text> thousand </xsl:text>
        <xsl:if test='$conjunctive and floor(floor(($number mod 1000) + 0.1) div 100) = 0'>and </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 1000) + 0.1)'/>
        </xsl:call-template>
      </xsl:when>
      <xsl:when test='$number mod 100 = 0'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 100)'/>
        </xsl:call-template>
        <xsl:text> hundred</xsl:text>
      </xsl:when>
      <xsl:when test='$number &gt;= 100'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 100)'/>
        </xsl:call-template>
        <xsl:text> hundred </xsl:text>
        <xsl:if test='$conjunctive'>and </xsl:if>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 100) + 0.1)'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:when test='$number = 1'>one</xsl:when>
      <xsl:when test='$number = 2'>two</xsl:when>
      <xsl:when test='$number = 3'>three</xsl:when>
      <xsl:when test='$number = 4'>four</xsl:when>
      <xsl:when test='$number = 5'>five</xsl:when>
      <xsl:when test='$number = 6'>six</xsl:when>
      <xsl:when test='$number = 7'>seven</xsl:when>
      <xsl:when test='$number = 8'>eight</xsl:when>
      <xsl:when test='$number = 9'>nine</xsl:when>
      <xsl:when test='$number = 10'>ten</xsl:when>
      <xsl:when test='$number = 11'>eleven</xsl:when>
      <xsl:when test='$number = 12'>twelve</xsl:when>
      <xsl:when test='$number = 13'>thirteen</xsl:when>
      <xsl:when test='$number = 14'>fourteen</xsl:when>
      <xsl:when test='$number = 15'>fifteen</xsl:when>
      <xsl:when test='$number = 16'>sixteen</xsl:when>
      <xsl:when test='$number = 17'>seventeen</xsl:when>
      <xsl:when test='$number = 18'>eighteen</xsl:when>
      <xsl:when test='$number = 19'>nineteen</xsl:when>
      <xsl:when test='$number = 20'>twenty</xsl:when>
      <xsl:when test='$number = 30'>thirty</xsl:when>
      <xsl:when test='$number = 40'>forty</xsl:when>
      <xsl:when test='$number = 50'>fifty</xsl:when>
      <xsl:when test='$number = 60'>sixty</xsl:when>
      <xsl:when test='$number = 70'>seventy</xsl:when>
      <xsl:when test='$number = 80'>eighty</xsl:when>
      <xsl:when test='$number = 90'>ninety</xsl:when>

      <xsl:when test='$number &lt; 100'>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor($number div 10) * 10'/>
        </xsl:call-template>
        <xsl:text> </xsl:text>
        <xsl:call-template name='math:number-as-word'>
          <xsl:with-param name='number' select='floor(($number mod 10) + 0.1)'/>
        </xsl:call-template>
      </xsl:when>
    </xsl:choose>
  </xsl:template>
</xsl:stylesheet>

