math.randomseed(os.time())

wrk.method = "GET"
wrk.headers["Connection"] = "keep-alive"

request = function()
  local r = math.random()
  if r < 0.5 then
    return wrk.format("GET", "/api/user")
  end

  local body = '{"item":"book"}'
  return wrk.format("POST", "/api/order", {
    ["Content-Type"] = "application/json",
    ["Content-Length"] = tostring(#body)
  }, body)
end
