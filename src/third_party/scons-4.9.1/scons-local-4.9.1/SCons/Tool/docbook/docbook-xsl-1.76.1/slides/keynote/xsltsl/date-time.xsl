<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:doc="http://xsltsl.org/xsl/documentation/1.0"
  xmlns:dt="http://xsltsl.org/date-time"
  xmlns:str="http://xsltsl.org/string"
  extension-element-prefixes="doc str">

  <doc:reference xmlns="">
    <referenceinfo>
      <releaseinfo role="meta">
        $Id: date-time.xsl 3991 2004-11-10 06:51:55Z balls $
      </releaseinfo>
      <author>
        <surname>Diamond</surname>
        <firstname>Jason</firstname>
      </author>
      <copyright>
        <year>2004</year>
        <holder>Steve Ball</holder>
      </copyright>
      <copyright>
        <year>2001</year>
        <holder>Jason Diamond</holder>
      </copyright>
    </referenceinfo>

    <title>Date/Time Processing</title>

    <partintro>
      <section>
        <title>Introduction</title>

        <para>This module provides templates for formatting and parsing date/time strings.</para>

        <para>See <ulink url="http://www.tondering.dk/claus/calendar.html">http://www.tondering.dk/claus/calendar.html</ulink> for more information on calendars and the calculations this library performs.</para>

      </section>
    </partintro>

  </doc:reference>

  <doc:template name="dt:format-date-time" xmlns="">
    <refpurpose>Returns a string with a formatted date/time.</refpurpose>

    <refdescription>
      <para>The formatted date/time is determined by the format parameter. The default format is %Y-%m-%dT%H:%M:%S%z, the W3C format.</para>
    </refdescription>

    <refparameter>
      <variablelist>

        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>The date-time value in XML Schemas (WXS) format.</para>
            <para>If this value is specified, it takes priority over other parameters.</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>year</term>
          <listitem>
            <para>Year, in either 2 or 4+ digit format..</para>
            <para>If the year is given as a two digit value, it will be converted to a four digit value using the fixed window method.  Values between 00 and 49 will be prepended by "20".  Values between 50 and 99 will be prepended by "19".</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; January = 1)</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>day</term>
          <listitem>
            <para>Day of month (1 - 31)</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>hour</term>
          <listitem>
            <para>Hours since midnight (0 - 23)</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>minute</term>
          <listitem>
            <para>Minutes after hour (0 - 59)</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>second</term>
          <listitem>
            <para>Seconds after minute (0 - 59)</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>time-zone</term>
          <listitem>
            <para>Time zone string (e.g., 'Z' or '-08:00')</para>
          </listitem>
        </varlistentry>

        <varlistentry>
          <term>format</term>
          <listitem>
            <para>The format specification.</para>
            <variablelist>

              <varlistentry>
                <term>%a</term>
                <listitem>
                  <para>Abbreviated weekday name</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%A</term>
                <listitem>
                  <para>Full weekday name</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%b</term>
                <listitem>
                  <para>Abbreviated month name</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%B</term>
                <listitem>
                  <para>Full month name</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%c</term>
                <listitem>
                  <para>Date and time representation appropriate for locale</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%d</term>
                <listitem>
                  <para>Day of month as decimal number (01 - 31)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%e</term>
                <listitem>
                  <para>Day of month as decimal number (1 - 31)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%H</term>
                <listitem>
                  <para>Hour in 24-hour format (00 - 23)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%I</term>
                <listitem>
                  <para>Hour in 12-hour format (01 - 12)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%i</term>
                <listitem>
                  <para>Hour in 12-hour format (1 - 12)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%j</term>
                <listitem>
                  <para>Day of year as decimal number (001 - 366)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%m</term>
                <listitem>
                  <para>Month as decimal number (01 - 12)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%n</term>
                <listitem>
                  <para>Month as decimal number (1 - 12)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%M</term>
                <listitem>
                  <para>Minute as decimal number (00 - 59)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%P</term>
                <listitem>
                  <para>Current locale's A.M./P.M. indicator for 12-hour clock, uppercase</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%Q</term>
                <listitem>
                  <para>Current locale's A.M./P.M. indicator for 12-hour clock, uppercase with periods</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%p</term>
                <listitem>
                  <para>Current locale's A.M./P.M. indicator for 12-hour clock, lowercase</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%q</term>
                <listitem>
                  <para>Current locale's A.M./P.M. indicator for 12-hour clock, lowercase with periods</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%S</term>
                <listitem>
                  <para>Second as decimal number (00 - 59)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%U</term>
                <listitem>
                  <para>Week of year as decimal number, with Sunday as first day of week (00 - 53)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%w</term>
                <listitem>
                  <para>Weekday as decimal number (0 - 6; Sunday is 0)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%W</term>
                <listitem>
                  <para>Week of year as decimal number, with Monday as first day of week (00 - 53)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%x</term>
                <listitem>
                  <para>Date representation for current locale </para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%X</term>
                <listitem>
                  <para>Time representation for current locale</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%y</term>
                <listitem>
                  <para>Year without century, as decimal number (00 - 99)</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%Y</term>
                <listitem>
                  <para>Year with century, as decimal number</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%z</term>
                <listitem>
                  <para>Time-zone name or abbreviation; no characters if time zone is unknown</para>
                </listitem>
              </varlistentry>

              <varlistentry>
                <term>%%</term>
                <listitem>
                  <para>Percent sign</para>
                </listitem>
              </varlistentry>

            </variablelist>
          </listitem>
        </varlistentry>

      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a formatted date/time string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:format-date-time">
    <xsl:param name='xsd-date-time'/>
    <xsl:param name="year"/>
    <xsl:param name="month"/>
    <xsl:param name="day"/>
    <xsl:param name="hour"/>
    <xsl:param name="minute"/>
    <xsl:param name="second"/>
    <xsl:param name="time-zone"/>
    <xsl:param name="format" select="'%Y-%m-%dT%H:%M:%S%z'"/>

    <xsl:value-of select="substring-before($format, '%')"/>

    <xsl:variable name="code" select="substring(substring-after($format, '%'), 1, 1)"/>

    <xsl:choose>

      <xsl:when test='$xsd-date-time'>
        <xsl:call-template name='dt:format-date-time'>
          <xsl:with-param name='year'>
            <xsl:call-template name='dt:get-xsd-datetime-year'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='month'>
            <xsl:call-template name='dt:get-xsd-datetime-month'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='day'>
            <xsl:call-template name='dt:get-xsd-datetime-day'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='hour'>
            <xsl:call-template name='dt:get-xsd-datetime-hour'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='minute'>
            <xsl:call-template name='dt:get-xsd-datetime-minute'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='second'>
            <xsl:call-template name='dt:get-xsd-datetime-second'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='time-zone'>
            <xsl:call-template name='dt:get-xsd-datetime-timezone'>
              <xsl:with-param name='xsd-date-time' select='$xsd-date-time'/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name='format'>
            <xsl:choose>
              <xsl:when test='contains($format, "%")'>
                <xsl:text>%</xsl:text>
                <xsl:value-of select='substring-after($format, "%")'/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select='$format'/>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:when>

      <!-- Abbreviated weekday name -->
      <xsl:when test="$code='a'">
        <xsl:variable name="day-of-the-week">
          <xsl:call-template name="dt:calculate-day-of-the-week">
            <xsl:with-param name="year" select="$year"/>
            <xsl:with-param name="month" select="$month"/>
            <xsl:with-param name="day" select="$day"/>
          </xsl:call-template>
        </xsl:variable>
        <xsl:call-template name="dt:get-day-of-the-week-abbreviation">
          <xsl:with-param name="day-of-the-week" select="$day-of-the-week"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Full weekday name -->
      <xsl:when test="$code='A'">
        <xsl:variable name="day-of-the-week">
          <xsl:call-template name="dt:calculate-day-of-the-week">
            <xsl:with-param name="year" select="$year"/>
            <xsl:with-param name="month" select="$month"/>
            <xsl:with-param name="day" select="$day"/>
          </xsl:call-template>
        </xsl:variable>
        <xsl:call-template name="dt:get-day-of-the-week-name">
          <xsl:with-param name="day-of-the-week" select="$day-of-the-week"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Abbreviated month name -->
      <xsl:when test="$code='b'">
        <xsl:call-template name="dt:get-month-abbreviation">
          <xsl:with-param name="month" select="$month"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Full month name -->
      <xsl:when test="$code='B'">
        <xsl:call-template name="dt:get-month-name">
          <xsl:with-param name="month" select="$month"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Date and time representation appropriate for locale -->
      <xsl:when test="$code='c'">
        <xsl:text>[not implemented]</xsl:text>
      </xsl:when>

      <!-- Day of month as decimal number (01 - 31) -->
      <xsl:when test="$code='d'">
        <xsl:if test="$day &lt; 10">0</xsl:if>
        <xsl:value-of select="number($day)"/>
      </xsl:when>
      <!-- Day of month as decimal number (1 - 31) -->
      <xsl:when test="$code='e'">
        <xsl:value-of select="number($day)"/>
      </xsl:when>

      <!-- Hour in 24-hour format (00 - 23) -->
      <xsl:when test="$code='H'">
        <xsl:if test="$hour &lt; 10">0</xsl:if>
        <xsl:value-of select="number($hour)"/>
      </xsl:when>

      <!-- Hour in 12-hour format (01 - 12) -->
      <xsl:when test="$code='I'">
        <xsl:choose>
          <xsl:when test="$hour = 0">12</xsl:when>
          <xsl:when test="$hour &lt; 10">0<xsl:value-of select="$hour - 0"/></xsl:when>
          <xsl:when test="$hour &lt; 13"><xsl:value-of select="$hour - 0"/></xsl:when>
          <xsl:when test="$hour &lt; 22">0<xsl:value-of select="$hour - 12"/></xsl:when>
          <xsl:otherwise><xsl:value-of select="$hour - 12"/></xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <!-- Hour in 12-hour format (1 - 12) -->
      <xsl:when test="$code='i'">
        <xsl:choose>
          <xsl:when test="$hour = 0">12</xsl:when>
          <xsl:when test="$hour &lt; 10"><xsl:value-of select="$hour - 0"/></xsl:when>
          <xsl:when test="$hour &lt; 13"><xsl:value-of select="$hour - 0"/></xsl:when>
          <xsl:when test="$hour &lt; 22"><xsl:value-of select="$hour - 12"/></xsl:when>
          <xsl:otherwise><xsl:value-of select="$hour - 12"/></xsl:otherwise>
        </xsl:choose>
      </xsl:when>

      <!-- Day of year as decimal number (001 - 366) -->
      <xsl:when test="$code='j'">
        <xsl:text>[not implemented]</xsl:text>
      </xsl:when>

      <!-- Month as decimal number (01 - 12) -->
      <xsl:when test="$code='m'">
        <xsl:if test="$month &lt; 10">0</xsl:if>
        <xsl:value-of select="number($month)"/>
      </xsl:when>
      <!-- Month as decimal number (1 - 12) -->
      <xsl:when test="$code='n'">
        <xsl:value-of select="number($month)"/>
      </xsl:when>

      <!-- Minute as decimal number (00 - 59) -->
      <xsl:when test="$code='M'">
        <xsl:if test="$minute &lt; 10">0</xsl:if>
        <xsl:value-of select="number($minute)"/>
      </xsl:when>

      <!-- Current locale's A.M./P.M. indicator for 12-hour clock -->
      <xsl:when test="$code='p'">
        <xsl:choose>
          <xsl:when test="$hour &lt; 12">am</xsl:when>
          <xsl:otherwise>pm</xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <!-- Current locale's A.M./P.M. indicator for 12-hour clock with periods -->
      <xsl:when test="$code='q'">
        <xsl:choose>
          <xsl:when test="$hour &lt; 12">am</xsl:when>
          <xsl:otherwise>p.m.</xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <!-- Current locale's A.M./P.M. indicator for 12-hour clock -->
      <xsl:when test="$code='P'">
        <xsl:choose>
          <xsl:when test="$hour &lt; 12">AM</xsl:when>
          <xsl:otherwise>PM</xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <!-- Current locale's A.M./P.M. indicator for 12-hour clock with periods -->
      <xsl:when test="$code='Q'">
        <xsl:choose>
          <xsl:when test="$hour &lt; 12">AM</xsl:when>
          <xsl:otherwise>P.M.</xsl:otherwise>
        </xsl:choose>
      </xsl:when>

      <!-- Second as decimal number (00 - 59) -->
      <xsl:when test="$code='S'">
        <xsl:if test="$second &lt; 10">0</xsl:if>
        <xsl:value-of select="number($second)"/>
      </xsl:when>

      <!-- Week of year as decimal number, with Sunday as first day of week (00 - 53) -->
      <xsl:when test="$code='U'">
        <!-- add 1 to day -->
        <xsl:call-template name="dt:calculate-week-number">
          <xsl:with-param name="year" select="$year"/>
          <xsl:with-param name="month" select="$month"/>
          <xsl:with-param name="day" select="$day + 1"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Weekday as decimal number (0 - 6; Sunday is 0) -->
      <xsl:when test="$code='w'">
        <xsl:call-template name="dt:calculate-day-of-the-week">
          <xsl:with-param name="year" select="$year"/>
          <xsl:with-param name="month" select="$month"/>
          <xsl:with-param name="day" select="$day"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Week of year as decimal number, with Monday as first day of week (00 - 53) -->
      <xsl:when test="$code='W'">
        <xsl:call-template name="dt:calculate-week-number">
          <xsl:with-param name="year" select="$year"/>
          <xsl:with-param name="month" select="$month"/>
          <xsl:with-param name="day" select="$day"/>
        </xsl:call-template>
      </xsl:when>

      <!-- Date representation for current locale -->
      <xsl:when test="$code='x'">
        <xsl:text>[not implemented]</xsl:text>
      </xsl:when>

      <!-- Time representation for current locale -->
      <xsl:when test="$code='X'">
        <xsl:text>[not implemented]</xsl:text>
      </xsl:when>

      <!-- Year without century, as decimal number (00 - 99) -->
      <xsl:when test="$code='y'">
        <xsl:choose>
          <xsl:when test='not(number($year))'>invalid year value</xsl:when>

          <!-- workaround MSXML bug -->
          <xsl:when test='number($year) mod 100 = 0'>00</xsl:when>

          <xsl:otherwise>
            <xsl:number format='01' value='number($year) mod 100'/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>

      <!-- Year with century, as decimal number -->
      <xsl:when test="$code='Y'">
        <xsl:choose>
          <xsl:when test='not(number($year))'>invalid year value</xsl:when>
          <xsl:when test='string-length($year) = 2'>
            <xsl:call-template name='dt:format-two-digit-year'>
              <xsl:with-param name='year' select='$year'/>
            </xsl:call-template>
          </xsl:when>
          <xsl:when test='string-length($year) >= 4'>
            <xsl:value-of select='$year'/>
          </xsl:when>
          <xsl:otherwise>invalid year value</xsl:otherwise>
        </xsl:choose>
      </xsl:when>

      <!-- Time-zone name or abbreviation; no characters if time zone is unknown -->
      <xsl:when test="$code='z'">
        <xsl:value-of select="$time-zone"/>
      </xsl:when>

      <!-- Percent sign -->
      <xsl:when test="$code='%'">
        <xsl:text>%</xsl:text>
      </xsl:when>

    </xsl:choose>

    <xsl:variable name="remainder" select="substring(substring-after($format, '%'), 2)"/>

    <xsl:if test="not($xsd-date-time) and $remainder">
      <xsl:call-template name="dt:format-date-time">
        <xsl:with-param name="year" select="$year"/>
        <xsl:with-param name="month" select="$month"/>
        <xsl:with-param name="day" select="$day"/>
        <xsl:with-param name="hour" select="$hour"/>
        <xsl:with-param name="minute" select="$minute"/>
        <xsl:with-param name="second" select="$second"/>
        <xsl:with-param name="time-zone" select="$time-zone"/>
        <xsl:with-param name="format" select="$remainder"/>
      </xsl:call-template>
    </xsl:if>

  </xsl:template>

  <doc:template name="dt:calculate-day-of-the-week" xmlns="">
    <refpurpose>Calculates the day of the week.</refpurpose>

    <refdescription>
      <para>Given any Gregorian date, this calculates the day of the week.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>year</term>
          <listitem>
            <para>Year</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; January = 1)</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>day</term>
          <listitem>
            <para>Day of month (1 - 31)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the day of the week (0 - 6; Sunday = 0).</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:calculate-day-of-the-week">
    <xsl:param name="year"/>
    <xsl:param name="month"/>
    <xsl:param name="day"/>

    <xsl:variable name="a" select="floor((14 - $month) div 12)"/>
    <xsl:variable name="y" select="$year - $a"/>
    <xsl:variable name="m" select="$month + 12 * $a - 2"/>

    <xsl:value-of select="($day + $y + floor($y div 4) - floor($y div 100) + floor($y div 400) + floor((31 * $m) div 12)) mod 7"/>

  </xsl:template>

  <doc:template name="dt:calculate-last-day-of-month" xmlns="">
    <refpurpose>Calculates the number of days for a specified month.</refpurpose>

    <refdescription>
      <para>Given any Gregorian month, this calculates the last day of the month.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>year</term>
          <listitem>
            <para>Year</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; January = 1)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the number of days in given month as a decimal number.</para>
    </refreturn>
  </doc:template>

  <xsl:template name="dt:calculate-last-day-of-month">
    <xsl:param name="year"/>
    <xsl:param name="month"/>

    <xsl:choose>
      <xsl:when test="$month = 2">
        <xsl:choose> 
          <xsl:when test="($year mod 4) = 0 and (($year mod 400) = 0
                          or ($year mod 100) != 0)">29</xsl:when>
          <xsl:otherwise>28</xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:when test="$month &lt; 8">
        <xsl:choose>
          <xsl:when test="$month mod 2 = 0">30</xsl:when>
          <xsl:otherwise>31</xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:otherwise>
        <xsl:choose>
          <xsl:when test="$month mod 2 = 1">30</xsl:when>
          <xsl:otherwise>31</xsl:otherwise>
        </xsl:choose>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-day-of-the-week-name" xmlns="">
    <refpurpose>Gets the day of the week's full name.</refpurpose>

    <refdescription>
      <para>Converts a numeric day of the week value into a string representing the day's full name.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>day-of-the-week</term>
          <listitem>
            <para>Day of the week (0 - 6; Sunday = 0)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:get-day-of-the-week-name">
    <xsl:param name="day-of-the-week"/>

    <xsl:choose>
      <xsl:when test="$day-of-the-week = 0">Sunday</xsl:when>
      <xsl:when test="$day-of-the-week = 1">Monday</xsl:when>
      <xsl:when test="$day-of-the-week = 2">Tuesday</xsl:when>
      <xsl:when test="$day-of-the-week = 3">Wednesday</xsl:when>
      <xsl:when test="$day-of-the-week = 4">Thursday</xsl:when>
      <xsl:when test="$day-of-the-week = 5">Friday</xsl:when>
      <xsl:when test="$day-of-the-week = 6">Saturday</xsl:when>
      <xsl:otherwise>error: <xsl:value-of select="$day-of-the-week"/></xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="dt:get-day-of-the-week-abbreviation" xmlns="">
    <refpurpose>Gets the day of the week's abbreviation.</refpurpose>

    <refdescription>
      <para>Converts a numeric day of the week value into a string representing the day's abbreviation.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>day-of-the-week</term>
          <listitem>
            <para>Day of the week (0 - 6; Sunday = 0)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:get-day-of-the-week-abbreviation">
    <xsl:param name="day-of-the-week"/>

    <xsl:choose>
      <xsl:when test="$day-of-the-week = 0">Sun</xsl:when>
      <xsl:when test="$day-of-the-week = 1">Mon</xsl:when>
      <xsl:when test="$day-of-the-week = 2">Tue</xsl:when>
      <xsl:when test="$day-of-the-week = 3">Wed</xsl:when>
      <xsl:when test="$day-of-the-week = 4">Thu</xsl:when>
      <xsl:when test="$day-of-the-week = 5">Fri</xsl:when>
      <xsl:when test="$day-of-the-week = 6">Sat</xsl:when>
      <xsl:otherwise>error: <xsl:value-of select="$day-of-the-week"/></xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="dt:get-month-name" xmlns="">
    <refpurpose>Gets the month's full name.</refpurpose>

    <refdescription>
      <para>Converts a numeric month value into a string representing the month's full name.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; Januaray = 1)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:get-month-name">
    <xsl:param name="month"/>

    <xsl:choose>
      <xsl:when test="$month = 1">January</xsl:when>
      <xsl:when test="$month = 2">February</xsl:when>
      <xsl:when test="$month = 3">March</xsl:when>
      <xsl:when test="$month = 4">April</xsl:when>
      <xsl:when test="$month = 5">May</xsl:when>
      <xsl:when test="$month = 6">June</xsl:when>
      <xsl:when test="$month = 7">July</xsl:when>
      <xsl:when test="$month = 8">August</xsl:when>
      <xsl:when test="$month = 9">September</xsl:when>
      <xsl:when test="$month = 10">October</xsl:when>
      <xsl:when test="$month = 11">November</xsl:when>
      <xsl:when test="$month = 12">December</xsl:when>
      <xsl:otherwise>error: <xsl:value-of select="$month"/></xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="dt:get-month-abbreviation" xmlns="">
    <refpurpose>Gets the month's abbreviation.</refpurpose>

    <refdescription>
      <para>Converts a numeric month value into a string representing the month's abbreviation.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; Januaray = 1)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns a string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:get-month-abbreviation">
    <xsl:param name="month"/>

    <xsl:choose>
      <xsl:when test="$month = 1">Jan</xsl:when>
      <xsl:when test="$month = 2">Feb</xsl:when>
      <xsl:when test="$month = 3">Mar</xsl:when>
      <xsl:when test="$month = 4">Apr</xsl:when>
      <xsl:when test="$month = 5">May</xsl:when>
      <xsl:when test="$month = 6">Jun</xsl:when>
      <xsl:when test="$month = 7">Jul</xsl:when>
      <xsl:when test="$month = 8">Aug</xsl:when>
      <xsl:when test="$month = 9">Sep</xsl:when>
      <xsl:when test="$month = 10">Oct</xsl:when>
      <xsl:when test="$month = 11">Nov</xsl:when>
      <xsl:when test="$month = 12">Dec</xsl:when>
      <xsl:otherwise>error: <xsl:value-of select="$month"/></xsl:otherwise>
    </xsl:choose>

  </xsl:template>

  <doc:template name="dt:calculate-julian-day" xmlns="">
    <refpurpose>Calculates the Julian Day for a specified date.</refpurpose>

    <refdescription>
      <para>Given any Gregorian date, this calculates the Julian Day.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>year</term>
          <listitem>
            <para>Year</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; January = 1)</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>day</term>
          <listitem>
            <para>Day of month (1 - 31)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the Julian Day as a decimal number.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:calculate-julian-day">
    <xsl:param name="year"/>
    <xsl:param name="month"/>
    <xsl:param name="day"/>

    <xsl:variable name="a" select="floor((14 - $month) div 12)"/>
    <xsl:variable name="y" select="$year + 4800 - $a"/>
    <xsl:variable name="m" select="$month + 12 * $a - 3"/>

    <xsl:value-of select="$day + floor((153 * $m + 2) div 5) + $y * 365 + floor($y div 4) - floor($y div 100) + floor($y div 400) - 32045"/>

  </xsl:template>

  <doc:template name="dt:format-julian-day" xmlns="">
    <refpurpose>Returns a string with a formatted date for a specified Julian Day.</refpurpose>

    <refdescription>
      <para>Given any Julian Day, this returns a string according to the format specification.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>julian-day</term>
          <listitem>
            <para>A Julian Day</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>format</term>
          <listitem>
            <para>The format specification. See dt:format-date-time for more details.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>A string.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:format-julian-day">
    <xsl:param name="julian-day"/>
    <xsl:param name="format" select="'%Y-%m-%d'"/>

    <xsl:variable name="a" select="$julian-day + 32044"/>
    <xsl:variable name="b" select="floor((4 * $a + 3) div 146097)"/>
    <xsl:variable name="c" select="$a - floor(($b * 146097) div 4)"/>

    <xsl:variable name="d" select="floor((4 * $c + 3) div 1461)"/>
    <xsl:variable name="e" select="$c - floor((1461 * $d) div 4)"/>
    <xsl:variable name="m" select="floor((5 * $e + 2) div 153)"/>

    <xsl:variable name="day" select="$e - floor((153 * $m + 2) div 5) + 1"/>
    <xsl:variable name="month" select="$m + 3 - 12 * floor($m div 10)"/>
    <xsl:variable name="year" select="$b * 100 + $d - 4800 + floor($m div 10)"/>

    <xsl:call-template name="dt:format-date-time">
      <xsl:with-param name="year" select="$year"/>
      <xsl:with-param name="month" select="$month"/>
      <xsl:with-param name="day" select="$day"/>
      <xsl:with-param name="format" select="$format"/>
    </xsl:call-template>

  </xsl:template>

  <doc:template name="dt:calculate-week-number" xmlns="">
    <refpurpose>Calculates the week number for a specified date.</refpurpose>

    <refdescription>
      <para>Assumes Monday is the first day of the week.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>year</term>
          <listitem>
            <para>Year</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month (1 - 12; January = 1)</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>day</term>
          <listitem>
            <para>Day of month (1 - 31)</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns the week number as a decimal number.</para>
    </refreturn>

  </doc:template>

  <xsl:template name="dt:calculate-week-number">
    <xsl:param name="year"/>
    <xsl:param name="month"/>
    <xsl:param name="day"/>

    <xsl:variable name="J">
      <xsl:call-template name="dt:calculate-julian-day">
        <xsl:with-param name="year" select="$year"/>
        <xsl:with-param name="month" select="$month"/>
        <xsl:with-param name="day" select="$day"/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:variable name="d4" select="($J + 31741 - ($J mod 7)) mod 146097 mod 36524 mod 1461"/>
    <xsl:variable name="L" select="floor($d4 div 1460)"/>
    <xsl:variable name="d1" select="(($d4 - $L) mod 365) + $L"/>

    <xsl:value-of select="floor($d1 div 7) + 1"/>

  </xsl:template>

  <doc:template name="dt:get-month-number" xmlns="">
    <refpurpose>Take a month by name and return a number which can be used as input to the templates. </refpurpose>

    <refdescription>
      <para>Input</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>month</term>
          <listitem>
            <para>Month as described either by full name or abbreviation.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Return a month as a decimal number. (Jan = 1)</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-month-number'>
    <xsl:param name='month'/>

    <xsl:variable name='monToUpper'>
      <xsl:call-template name='str:to-upper'>
        <xsl:with-param name='text' select='$month'/>
      </xsl:call-template>
    </xsl:variable>

    <xsl:choose>
      <xsl:when test='starts-with($monToUpper,"JAN")'>
        <xsl:value-of select='"1"'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"FEB")'>
        <xsl:value-of select='2'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"MAR")'>
        <xsl:value-of select='3'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"APR")'>
	<xsl:value-of select='4'/>
      </xsl:when>      
      <xsl:when test='starts-with($monToUpper,"MAY")'>
	<xsl:value-of select='5'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"JUN")'>
	<xsl:value-of select='6'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"JUL")'>
	<xsl:value-of select='7'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"AUG")'>
	<xsl:value-of select='8'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"SEP")'>
	<xsl:value-of select='9'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"OCT")'>
	<xsl:value-of select='10'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"NOV")'>
	<xsl:value-of select='11'/>
      </xsl:when>
      <xsl:when test='starts-with($monToUpper,"DEC")'>
	<xsl:value-of select='"12"'/>
      </xsl:when>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-year" xmlns="">
    <refpurpose>Return year component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns year component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-year'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-year'>
          <xsl:with-param name='xsd-date-time' select='substring-before($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'/>

      <xsl:otherwise>
        <!-- This is a date -->
        <xsl:value-of select='substring-before($xsd-date-time, "-")'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-month" xmlns="">
    <refpurpose>Return month component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns month component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-month'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-month'>
          <xsl:with-param name='xsd-date-time' select='substring-before($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'/>

      <xsl:otherwise>
        <!-- This is a date -->
        <xsl:value-of select='substring(substring-after($xsd-date-time, "-"), 1, 2)'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-day" xmlns="">
    <refpurpose>Return day component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns day component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-day'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-day'>
          <xsl:with-param name='xsd-date-time' select='substring-before($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'/>

      <xsl:otherwise>
        <!-- This is a date -->
        <xsl:value-of select='substring(substring-after($xsd-date-time, "-"), 4, 2)'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-hour" xmlns="">
    <refpurpose>Return hour component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns hour component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-hour'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-hour'>
          <xsl:with-param name='xsd-date-time' select='substring-after($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'>
        <xsl:value-of select='substring($xsd-date-time, 1, 2)'/>
      </xsl:when>

      <xsl:otherwise>
        <!-- This is a date -->
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-minute" xmlns="">
    <refpurpose>Return minute component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns minute component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-minute'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-minute'>
          <xsl:with-param name='xsd-date-time' select='substring-after($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'>
        <xsl:value-of select='substring($xsd-date-time, 4, 2)'/>
      </xsl:when>

      <xsl:otherwise>
        <!-- This is a date -->
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-second" xmlns="">
    <refpurpose>Return second component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns second component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-second'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-second'>
          <xsl:with-param name='xsd-date-time' select='substring-after($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <!-- Check for time -->
      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'>
        <xsl:variable name='part' select='substring($xsd-date-time, 7)'/>
        <xsl:choose>
          <xsl:when test='contains($part, "Z")'>
            <xsl:value-of select='substring-before($part, "Z")'/>
          </xsl:when>
          <xsl:when test='contains($part, "+")'>
            <xsl:value-of select='substring-before($part, "+")'/>
          </xsl:when>
          <xsl:when test='contains($part, "-")'>
            <xsl:value-of select='substring-before($part, "-")'/>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select='$part'/>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>

      <xsl:otherwise>
        <!-- This is a date -->
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:get-xsd-datetime-timezone" xmlns="">
    <refpurpose>Return timezone component of XSD DateTime value.</refpurpose>

    <refdescription>
      <para>Extract component of XML Schemas DateTime value.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>xsd-date-time</term>
          <listitem>
            <para>A value in XSD DateTime format.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns timezone component.</para>
    </refreturn>
  </doc:template>

  <xsl:template name='dt:get-xsd-datetime-timezone'>
    <xsl:param name='xsd-date-time'/>

    <xsl:choose>
      <xsl:when test='contains($xsd-date-time, "Z")'>Z</xsl:when>
      <xsl:when test='contains($xsd-date-time, "T")'>
        <xsl:call-template name='dt:get-xsd-datetime-timezone'>
          <xsl:with-param name='xsd-date-time' select='substring-after($xsd-date-time, "T")'/>
        </xsl:call-template>
      </xsl:when>

      <xsl:when test='substring($xsd-date-time, 3, 1) = ":"'>
        <!-- This is a time -->
        <xsl:choose>
          <xsl:when test='contains($xsd-date-time, "+")'>
            <xsl:text>+</xsl:text>
            <xsl:value-of select='substring-after($xsd-date-time, "+")'/>
          </xsl:when>
          <xsl:when test='contains($xsd-date-time, "-")'>
            <xsl:text>-</xsl:text>
            <xsl:value-of select='substring-after($xsd-date-time, "-")'/>
          </xsl:when>
        </xsl:choose>
      </xsl:when>
      <xsl:otherwise>
        <!-- This is a date -->
        <xsl:value-of select='substring(substring-after($xsd-date-time, "-"), 6)'/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <doc:template name="dt:format-two-digit-year" xmlns="">
    <refpurpose>Return two digit year as four digit year value.</refpurpose>

    <refdescription>
      <para>Prepend century to two digit year value.</para>
      <para>Century value is calculated according to suggested solutions in RFC2626 (section 5).</para>
      <para>Fixed window solution: 20 is prepended to year if the year is less than 50, otherwise 19 is prepended to year.</para>
      <para>Sliding window solution: The year is considered in the future if the year is less than the current 2 digit year plus 'n' years (where 'n' is a param), otherwise it is considered in the past.</para>
    </refdescription>

    <refparameter>
      <variablelist>
        <varlistentry>
          <term>year</term>
          <listitem>
            <para>A year value in 2 digit format.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>method</term>
          <listitem>
            <para>RFC2626 suggested solution ('fixed' or 'sliding').  Default is 'fixed'.</para>
          </listitem>
        </varlistentry>
        <varlistentry>
          <term>n</term>
          <listitem>
            <para>No. of years. Used in sliding windows solution.</para>
          </listitem>
        </varlistentry>
      </variablelist>
    </refparameter>

    <refreturn>
      <para>Returns four digit year value.</para>
    </refreturn>
  </doc:template>
  
  <xsl:template name="dt:format-two-digit-year">
    <xsl:param name="year" />
    <xsl:param name="method" select="'fixed'"/>

    <xsl:choose>
      <xsl:when test="string-length($year) != 2">invalid year value</xsl:when>
      <xsl:when test="$method = 'fixed'">
        <xsl:choose>
          <xsl:when test="$year &lt; 50">20</xsl:when>
          <xsl:otherwise>19</xsl:otherwise>
        </xsl:choose>
        <xsl:value-of select="$year" />
      </xsl:when>
      <xsl:when test="$method = 'window'">not yet implemented</xsl:when>
      <xsl:otherwise>invalid method</xsl:otherwise>
    </xsl:choose>
  </xsl:template>

 </xsl:stylesheet>
