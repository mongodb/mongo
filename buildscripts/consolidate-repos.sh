#!/bin/sh
#
# consolidate-repos.sh
#
# Create new repo directory under /var/www/repo.consolidated
# containing every deb and every rpm under /var/www/ with proper
# repo metadata for Debian and Ubuntu
#

source_dir=/var/www

repodir=/var/www/repo.consolidated

gpg_recip='<richard@10gen.com>' 

echo "Using directory: $repodir"

tempfile=`mktemp /tmp/consolidate-repos.XXXXXX`

mkdir -p "$repodir"

find "$source_dir" -name \*.rpm -o -name \*.deb -o -name Release | grep -v "$repodir" > "$tempfile"

echo "Scanning and copying package files from $source_dir"
echo ". = skipping existing file, @ = copying file"
while read package
do
  new_package_location="$repodir`echo \"$package\" | sed 's/\/var\/www\/[^\/]*//;'`"

  # skip if the directory structure looks weird
  #
  if [ "`echo \"$new_package_location\" | grep /repo/`" ]
  then
    continue
  fi  

  # skip if it's already there 
  #
  if [ -e "$new_package_location" -a "`basename \"$package\"`" != "Release" ]
  then
      echo -n .
  else
    mkdir -p "`dirname \"$new_package_location\"`"
    echo -n @
    cp "$package" "$new_package_location"
  fi
done < "$tempfile"
echo

# packages are in place, now create metadata
#
for debian_dir in "$repodir"/ubuntu-* "$repodir"/debian-* 
do
  cd "$debian_dir" 
  for arch_dir in dists/dist/10gen/*   
  do
    echo "Generating Packages file under $debian_dir/$arch_dir"
    dpkg-scanpackages --multiversion "$arch_dir"   > "$arch_dir"/Packages
    gzip -9c  "$arch_dir"/Packages >  "$arch_dir"/Packages.gz
  done

  for release_dir in "$debian_dir"/dists/dist
  do
    echo "Generating Release file under $release_dir"
    cd $release_dir
    tempfile=`mktemp /tmp/ReleaseXXXXXX`
    tempfile2=`mktemp /tmp/ReleaseXXXXXX`
    mv Release $tempfile
    head -9 $tempfile > $tempfile2
    apt-ftparchive release . >> $tempfile2
    cp $tempfile2 Release
    chmod 644 Release
    rm Release.gpg
    echo "Signing Release file"
    gpg -r "$gpg_recip" --no-secmem-warning -abs --output Release.gpg  Release
  done
done

for redhat_dir in "$repodir"/redhat/os/*
do
  echo "Generating redhat repo metadata under $redhat_dir"
  cd "$redhat_dir"
  createrepo .
done


