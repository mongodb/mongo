#!/bin/bash
#
# consolidate-repos.sh
#
# Create new repo directory under /var/www-org/repo.consolidated
# containing every deb and every rpm under /var/www-org/ with proper
# repo metadata for apt and yum
#

source_dir=/var/www-org/

repodir=/var/www-org/repo.consolidated

gpg_recip='<richard@10gen.com>'

stable_branch="2.6"
unstable_branch="2.7"

echo "Using directory: $repodir"

# set up repo dirs if they don't exist
#
mkdir -p "$repodir/apt/ubuntu"
mkdir -p "$repodir/apt/debian"
mkdir -p "$repodir/yum/redhat"

# to support different $releasever values in yum repo configurations
#
if [ ! -e "$repodir/yum/redhat/6Server" ]
then
  ln -s 6 "$repodir/yum/redhat/6Server"
fi

if [ ! -e "$repodir/yum/redhat/7Server" ]
then
  ln -s 7 "$repodir/yum/redhat/7Server"
fi

if [ ! -e "$repodir/yum/redhat/5Server" ]
then
  ln -s 5 "$repodir/yum/redhat/5Server"
fi

echo "Scanning and copying package files from $source_dir"
echo ". = skipping existing file, @ = copying file"
for package in $(find "$source_dir" -not \( -path "$repodir" -prune \) -and \( -name \*.rpm -o -name \*.deb -o -name Release \))
do
  new_package_location="$repodir$(echo "$package" | sed 's/\/var\/www-org\/[^\/]*//;')"
  # skip if the directory structure looks weird
  #
  if echo "$new_package_location" | grep -q /repo/
  then
    continue
  fi

  # skip if not community package
  #
  if ! echo "$new_package_location" | grep -q org
  then
    continue
  fi
  # skip if it's already there
  #
  if [ -e "$new_package_location" -a "$(basename "$package")" != "Release" ]
  then
      echo -n .
  else
    mkdir -p "$(dirname "$new_package_location")"
    echo -n @
    cp "$package" "$new_package_location"
  fi
done
echo

# packages are in place, now create metadata
#
for debian_dir in "$repodir"/apt/ubuntu "$repodir"/apt/debian
do
  cd "$debian_dir"
  for section_dir in $(find dists -type d -name multiverse -o -name main)
  do
    for arch_dir in "$section_dir"/{binary-i386,binary-amd64}
    do
      echo "Generating Packages file under $debian_dir/$arch_dir"
      if [ ! -d $arch_dir ]
      then
        mkdir $arch_dir
      fi
      dpkg-scanpackages --multiversion "$arch_dir"   > "$arch_dir"/Packages
      gzip -9c  "$arch_dir"/Packages >  "$arch_dir"/Packages.gz
    done
  done

  for release_file in $(find "$debian_dir" -name Release)
  do
    release_dir=$(dirname "$release_file")
    echo "Generating Release file under $release_dir"
    cd $release_dir
    tempfile=$(mktemp /tmp/ReleaseXXXXXX)
    tempfile2=$(mktemp /tmp/ReleaseXXXXXX)
    mv Release $tempfile
    head -7 $tempfile > $tempfile2
    apt-ftparchive release . >> $tempfile2
    cp $tempfile2 Release
    chmod 644 Release
    rm Release.gpg
    echo "Signing Release file"
    gpg -r "$gpg_recip" --no-secmem-warning -abs --output Release.gpg  Release
  done
done

# Create symlinks for stable and unstable branches
#
# Examples:
#
# /var/www-org/repo.consolidated/yum/redhat/5/mongodb-org/unstable -> 2.5
# /var/www-org/repo.consolidated/yum/redhat/6/mongodb-org/unstable -> 2.5
# /var/www-org/repo.consolidated/apt/ubuntu/dists/precise/mongodb-org/unstable -> 2.5
# /var/www-org/repo.consolidated/apt/debian/dists/wheezy/mongodb-org/unstable -> 2.5
#
for unstable_branch_dir in "$repodir"/yum/redhat/*/*/$unstable_branch "$repodir"/yum/amazon/*/*/$unstable_branch "$repodir"/apt/debian/dists/*/*/$unstable_branch "$repodir"/apt/ubuntu/dists/*/*/$unstable_branch "$repodir"/zypper/suse/*/*/$unstable_branch
do
  full_unstable_path=$(dirname "$unstable_branch_dir")/unstable
  if [ -e "$unstable_branch_dir" -a ! -e "$full_unstable_path" ]
  then
    echo "Linking unstable branch directory $unstable_branch_dir to $full_unstable_path"
    ln -s $unstable_branch $full_unstable_path
  fi
done

for stable_branch_dir in "$repodir"/yum/redhat/*/*/$stable_branch "$repodir"/yum/amazon/*/*/$stable_branch "$repodir"/apt/debian/dists/*/*/$stable_branch "$repodir"/apt/ubuntu/dists/*/*/$stable_branch "$repodir"/zypper/suse/*/*/$stable_branch
do
  full_stable_path=$(dirname "$stable_branch_dir")/stable
  if [ -e "$stable_branch_dir" -a ! -e "$full_stable_path" ]
  then
    echo "Linking stable branch directory $stable_branch_dir to $full_stable_path"
    ln -s $stable_branch $full_stable_path
  fi
done

for rpm_dir in $(find "$repodir"/yum/redhat "$repodir"/yum/amazon "$repodir"/zypper/suse -type d -name x86_64 -o -name i386)
do
  echo "Generating redhat repo metadata under $rpm_dir"
  cd "$rpm_dir"
  createrepo .
done
