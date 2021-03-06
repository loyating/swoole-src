--TEST--
swoole_http_server: raw-cookie
--SKIPIF--
<?php
require __DIR__ . '/../include/skipif.inc';
skip_if_in_valgrind();
?>
--FILE--
<?php
require __DIR__ . '/../include/bootstrap.php';
require __DIR__ . '/../include/api/swoole_http_client/simple_http_client.php';

$simple_http_server = __DIR__ . "/../include/api/swoole_http_server/simple_http_server.php";
$closeServer = start_server($simple_http_server, HTTP_SERVER_HOST, $port = get_one_free_port());

$rawcontent = "HELLO";
testRawCookie(HTTP_SERVER_HOST, $port, $rawcontent, function(\swoole_http_client $cli) use($closeServer, $rawcontent) {
    assert($cli->headers["set-cookie"] === "rawcontent=$rawcontent");
    echo "SUCCESS";
    $closeServer();
});

suicide(1000, SIGTERM, $closeServer);
?>
--EXPECT--
SUCCESS