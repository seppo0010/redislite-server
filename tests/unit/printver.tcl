start_server {} {
    set i [r info]
    regexp {redislite_version:(.*?)\r\n} $i - version
    regexp {redislite_git_sha1:(.*?)\r\n} $i - sha1
    puts "Testing Redislite version $version ($sha1)"
}
