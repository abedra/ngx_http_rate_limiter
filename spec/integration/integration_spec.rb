require 'spec_helper'

describe "Integration Specs" do

  before do
    @redis = Redis.new
    @redis.flushdb
  end

  after  { @redis.flushdb }

  describe "Bootstrap" do
    it "Redis is running" do
      expect(@redis.ping).to eq("PONG")
    end

    it "Webserver is running" do
      expect(Curl.get("http://127.0.0.1:8888").response_code).to eq(200)
    end
  end

  describe "Rate Limiter" do
    it "returns a 429 after the rate limit is exceeded" do
      11.times do
        expect(Curl.get("http://127.0.0.1:8888").response_code).to eq(200)
      end

      expect(Curl.get("http://127.0.0.1:8888").response_code).to eq(429)
    end
  end
end
