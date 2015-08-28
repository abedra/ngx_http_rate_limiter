# NGINX Rate Limiter Module

## What is it?

This module is a Redis backed rate limit module for NGINX web
servers. It allows a group of web servers to share a global state of
connection attempts from an actor.

## How does it work?

If the actor has exceeded the rate limit the module returns a 429
status code. Here's a before and after example:

```
$ curl -I localhost:8888
HTTP/1.1 200 OK
Server: nginx/1.8.0
Date: Fri, 28 Aug 2015 21:41:35 GMT
Content-Type: text/html
Content-Length: 612
Last-Modified: Fri, 28 Aug 2015 17:15:44 GMT
Connection: keep-alive
X-Rate-Limit-Remaining: 9
X-Rate-Limit-Limit: 10
X-Rate-Limit-Reset: 60
ETag: "55e09740-264"
Accept-Ranges: bytes
```

```
curl -I localhost:8888
HTTP/1.1 429
Server: nginx/1.8.0
Date: Fri, 28 Aug 2015 21:41:41 GMT
Content-Length: 0
Connection: keep-alive
X-Rate-Limit-Reset: 54
```

## Dependencies

* [hiredis](https://github.com/redis/hiredis) 0.13.1 or greater
* [Redis](http://redis.io) 2.8 or greater (runtime only)

#### Installation

Since NGINX doesn't have shared module support, this module will need
to be accessible during the compilation of NGINX itself. The `scripts`
folder has some examples on how to add the module to your NGINX
installation.

To activate and configure this module you will need to set some
directives. The following list explains what each directive is and
what is does. At the moment, the Repsheet directives all live under
the main configuration section of `nginx.conf`.

* `rate_limiter_rate_limit <n>` - The maximum number of requests allowed in the given window
* `rate_limiter_window_size <n>` - The period (in minutes) until the limit resets
* `rate_limiter_redis_host <host>` - Sets the host for the Redis connection
* `rate_limiter_redis_port <port>` - Sets the port for the Redis connection

Here's a simple example NGINX config:

```
events {
  worker_connections  1024;
}

http {
  rate_limiter_rate_limit 10;
  rate_limiter_window_size 1;
  rate_limiter_redis_host localhost;
  rate_limiter_redis_port 6379;

  server {
    listen 8888;
    location / {

    }
  }
}
```

## Running the Integration Tests

This project comes with a basic set of integration tests to ensure
that things are working. If you want to run the tests, you will need
to have [Ruby](http://www.ruby-lang.org/en/),
[RubyGems](http://rubygems.org/), and [Bundler](http://bundler.io/)
installed. In order to run the integration tests, use the following
commands:

```sh
bundle install
script/bootstrap
rake nginx:compile
rake
```

The `script/bootstrap` task will take some time. It downloads and
compiles NGINX, and then configures everything to work
together. Running `rake` launches some curl based tests that hit the
site and exercise Repsheet, then test that everything is working as
expected.
