#! /bin/ksh

# '@(#)tzselect.ksh	8.1'

# Ask the user about the time zone, and output the resulting TZ value to stdout.
# Interact with the user via stderr and stdin.

# Contributed by Paul Eggert.

# Porting notes:
#
# This script requires several features of the Korn shell.
# If your host lacks the Korn shell,
# you can use either of the following free programs instead:
#
#	<a href=ftp://ftp.gnu.org/pub/gnu/>
#	Bourne-Again shell (bash)
#	</a>
#
#	<a href=ftp://ftp.cs.mun.ca/pub/pdksh/pdksh.tar.gz>
#	Public domain ksh
#	</a>
#
# This script also uses several features of modern awk programs.
# If your host lacks awk, or has an old awk that does not conform to Posix.2,
# you can use either of the following free programs instead:
#
#	<a href=ftp://ftp.gnu.org/pub/gnu/>
#	GNU awk (gawk)
#	</a>
#
#	<a href=ftp://ftp.whidbey.net/pub/brennan/>
#	mawk
#	</a>


# Specify default values for environment variables if they are unset.
: ${AWK=awk}
: ${TZDIR=$(pwd)}

# Check for awk Posix compliance.
($AWK -v x=y 'BEGIN { exit 123 }') </dev/null >/dev/null 2>&1
[ $? = 123 ] || {
	echo >&2 "$0: Sorry, your \`$AWK' program is not Posix compatible."
	exit 1
}

# Make sure the tables are readable.
TZ_COUNTRY_TABLE=$TZDIR/iso3166.tab
TZ_ZONE_TABLE=$TZDIR/zone.tab
for f in $TZ_COUNTRY_TABLE $TZ_ZONE_TABLE
do
	<$f || {
		echo >&2 "$0: time zone files are not set up correctly"
		exit 1
	}
done

newline='
'
IFS=$newline


# Work around a bug in bash 1.14.7 and earlier, where $PS3 is sent to stdout.
case $(echo 1 | (select x in x; do break; done) 2>/dev/null) in
?*) PS3=
esac


# Begin the main loop.  We come back here if the user wants to retry.
while

	echo >&2 'Please identify a location' \
		'so that time zone rules can be set correctly.'

	continent=
	country=
	region=


	# Ask the user for continent or ocean.

	echo >&2 'Please select a continent or ocean.'

	select continent in \
	    Africa \
	    Americas \
	    Antarctica \
	    'Arctic Ocean' \
	    Asia \
	    'Atlantic Ocean' \
	    Australia \
	    Europe \
	    'Indian Ocean' \
	    'Pacific Ocean' \
	    'none - I want to specify the time zone using the Posix TZ format.'
	do
	    case $continent in
	    '')
		echo >&2 'Please enter a number in range.';;
	    ?*)
		case $continent in
		Americas) continent=America;;
		*' '*) continent=$(expr "$continent" : '\([^ ]*\)')
		esac
		break
	    esac
	done
	case $continent in
	'')
		exit 1;;
	none)
		# Ask the user for a Posix TZ string.  Check that it conforms.
		while
			echo >&2 'Please enter the desired value' \
				'of the TZ environment variable.'
			echo >&2 'For example, GST-10 is a zone named GST' \
				'that is 10 hours ahead (east) of UTC.'
			read TZ
			$AWK -v TZ="$TZ" 'BEGIN {
				tzname = "[^-+,0-9][^-+,0-9][^-+,0-9]+"
				time = "[0-2]?[0-9](:[0-5][0-9](:[0-5][0-9])?)?"
				offset = "[-+]?" time
				date = "(J?[0-9]+|M[0-9]+\.[0-9]+\.[0-9]+)"
				datetime = "," date "(/" time ")?"
				tzpattern = "^(:.*|" tzname offset "(" tzname \
				  "(" offset ")?(" datetime datetime ")?)?)$"
				if (TZ ~ tzpattern) exit 1
				exit 0
			}'
		do
			echo >&2 "\`$TZ' is not a conforming" \
				'Posix time zone string.'
		done
		TZ_for_date=$TZ;;
	*)
		# Get list of names of countries in the continent or ocean.
		countries=$($AWK -F'\t' \
			-v continent="$continent" \
			-v TZ_COUNTRY_TABLE="$TZ_COUNTRY_TABLE" \
		'
			/^#/ { next }
			$3 ~ ("^" continent "/") {
				if (!cc_seen[$1]++) cc_list[++ccs] = $1
			}
			END {
				while (getline <TZ_COUNTRY_TABLE) {
					if ($0 !~ /^#/) cc_name[$1] = $2
				}
				for (i = 1; i <= ccs; i++) {
					country = cc_list[i]
					if (cc_name[country]) {
					  country = cc_name[country]
					}
					print country
				}
			}
		' <$TZ_ZONE_TABLE | sort -f)


		# If there's more than one country, ask the user which one.
		case $countries in
		*"$newline"*)
			echo >&2 'Please select a country.'
			select country in $countries
			do
			    case $country in
			    '') echo >&2 'Please enter a number in range.';;
			    ?*) break
			    esac
			done

			case $country in
			'') exit 1
			esac;;
		*)
			country=$countries
		esac


		# Get list of names of time zone rule regions in the country.
		regions=$($AWK -F'\t' \
			-v country="$country" \
			-v TZ_COUNTRY_TABLE="$TZ_COUNTRY_TABLE" \
		'
			BEGIN {
				cc = country
				while (getline <TZ_COUNTRY_TABLE) {
					if ($0 !~ /^#/  &&  country == $2) {
						cc = $1
						break
					}
				}
			}
			$1 == cc { print $4 }
		' <$TZ_ZONE_TABLE)


		# If there's more than one region, ask the user which one.
		case $regions in
		*"$newline"*)
			echo >&2 'Please select one of the following' \
				'time zone regions.'
			select region in $regions
			do
				case $region in
				'') echo >&2 'Please enter a number in range.';;
				?*) break
				esac
			done
			case $region in
			'') exit 1
			esac;;
		*)
			region=$regions
		esac

		# Determine TZ from country and region.
		TZ=$($AWK -F'\t' \
			-v country="$country" \
			-v region="$region" \
			-v TZ_COUNTRY_TABLE="$TZ_COUNTRY_TABLE" \
		'
			BEGIN {
				cc = country
				while (getline <TZ_COUNTRY_TABLE) {
					if ($0 !~ /^#/  &&  country == $2) {
						cc = $1
						break
					}
				}
			}
			$1 == cc && $4 == region { print $3 }
		' <$TZ_ZONE_TABLE)

		# Make sure the corresponding zoneinfo file exists.
		TZ_for_date=$TZDIR/$TZ
		<$TZ_for_date || {
			echo >&2 "$0: time zone files are not set up correctly"
			exit 1
		}
	esac


	# Use the proposed TZ to output the current date relative to UTC.
	# Loop until they agree in seconds.
	# Give up after 8 unsuccessful tries.

	extra_info=
	for i in 1 2 3 4 5 6 7 8
	do
		TZdate=$(LANG=C TZ="$TZ_for_date" date)
		UTdate=$(LANG=C TZ=UTC0 date)
		TZsec=$(expr "$TZdate" : '.*:\([0-5][0-9]\)')
		UTsec=$(expr "$UTdate" : '.*:\([0-5][0-9]\)')
		case $TZsec in
		$UTsec)
			extra_info="
Local time is now:	$TZdate.
Universal Time is now:	$UTdate."
			break
		esac
	done


	# Output TZ info and ask the user to confirm.

	echo >&2 ""
	echo >&2 "The following information has been given:"
	echo >&2 ""
	case $country+$region in
	?*+?*)	echo >&2 "	$country$newline	$region";;
	?*+)	echo >&2 "	$country";;
	+)	echo >&2 "	TZ='$TZ'"
	esac
	echo >&2 ""
	echo >&2 "Therefore TZ='$TZ' will be used.$extra_info"
	echo >&2 "Is the above information OK?"

	ok=
	select ok in Yes No
	do
	    case $ok in
	    '') echo >&2 'Please enter 1 for Yes, or 2 for No.';;
	    ?*) break
	    esac
	done
	case $ok in
	'') exit 1;;
	Yes) break
	esac
do :
done

case $SHELL in
*csh) file=.login line="setenv TZ '$TZ'";;
*) file=.profile line="TZ='$TZ'; export TZ"
esac

echo >&2 "
You can make this change permanent for yourself by appending the line
	$line
to the file '$file' in your home directory; then log out and log in again.

Here is that TZ value again, this time on standard output so that you
can use the $0 command in shell scripts:"

echo "$TZ"
