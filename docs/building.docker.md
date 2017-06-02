# Building MongoDB with Docker

Assuming you have [docker](http://www.docker.io/gettingstarted/#h_installation) and [git](http://git-scm.com/) installed, you can build mongo using the following commands;

```bash
git clone https://github.com/mongodb/mongo.git
cd mongo
docker build -t=mongo .
```