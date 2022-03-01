# Upgrading MozJS

Follow these steps to upgrade MozJS. This is supposed to work on minor updates of
`MozJS ESR 91`.

**Note:** You first need to prepare the MozJS repo and make sure that you have an upgraded branch
under the [mongodb-forks/spidermonkey](https://github.com/mongodb-forks/spidermonkey) repo.

## Step 1: Update the MozJS version

Edit `get-sources.sh` and update the `LIB_GIT_REVISION` variable with the branch name of
the new version of MozJS.

## Step 2: Make sure you have `autoconf2.13` installed.

Check whether you have `autoconf2.13` installed by running either of the
`autoconf2.13 --version`, `autoconf213 --version` or `autoconf-2.13 --version` commands depending on
your target platform.

If not installed, you can install it like this:

#### On Linux:

```
sudo apt-get install autoconf2.13
```

#### On macOS:

```
brew install autoconf@2.13
```

#### On other platforms (build from source):

```
wget ftp://ftp.gnu.org/gnu/autoconf/autoconf-2.13.tar.gz
tar -xf autoconf-2.13.tar.gz
cd autoconf-2.13
./configure --prefix=$HOME/local
make
make install
cp $HOME/local/bin/autoconf $HOME/local/bin/autoconf2.13
PATH="$PATH:$HOME/local/bin"
export PATH
```

## Step 2: Checkout the branch of `mongo` repository used for MozJS upgrade

```
cd $HOME
git clone git@github.com:10gen/mongo.git
cd mongo
git checkout -b  <your_branch_name> origin/<your_branch_name>
```

## Step 3: Get MozJS sources

```
cd $HOME/mongo/src/third_party/mozjs
rm -rf mozilla-release
./get-sources.sh
```

## Step 4: Extract MozJS sources

```
cd $HOME/mongo/src/third_party/mozjs
rm -rf extract include
./extract.sh
```

## Step 5: Generate platform-specific code

Run one of the following invocations of the 'gen-config.sh' script for your target platform:


### Linux (x86_64)

```
cd $HOME/mongo/src/third_party/mozjs
./gen-config.sh x86_64 linux
```

### macOS (x86_64)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh x86_64 macOS
```

### Windows (x86_64)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh x86_64 windows
```

### Linux (aarch64)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh aarch64 linux
```

### macOS (aarch64)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh aarch64 macOS
```

### Linux (ppc64le)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh ppc64le linux
```

### Linux (s390x)

```
cd $HOME/mongo/src/third_party/mozjs
# ./gen-config.sh s390x linux
```

You need to run through all of these steps once for each supported platform.
