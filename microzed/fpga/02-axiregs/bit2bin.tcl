# bit2bin: convert a Vivado .bit file into the .bin the Zynq FPGA manager loads.
#
# A .bit file = a short text header (design name, part, date) + the raw
# configuration data. The Zynq PCAP (the PS block that programs the PL) wants
# only the data, with every 32-bit word byte-swapped (little-endian), so the
# sync word AA 99 55 66 appears as 66 55 99 AA. The kernel driver checks for
# that ("Invalid bitstream, could not find a sync word" otherwise).
#
# Same result as AMD's "bootgen -arch zynq -process_bitstream bin", which
# needs Vitis.
#
# Use from Vivado's Tcl (build.tcl sources it), or on its own:
#   tclsh bit2bin.tcl in.bit out.bin

proc bit2bin {bitfile binfile} {
    set f [open $bitfile rb]
    set d [read $f]
    close $f

    # Field 1: 2-byte length + magic bytes. Field 2: 2-byte length (1) + key 'a'.
    binary scan $d Su len
    set pos [expr {2 + $len}]
    binary scan $d @${pos}Su len
    incr pos 2
    set key [string index $d [expr {$pos + $len - 1}]]
    incr pos $len

    # Keys a-d: 2-byte length + text. Key e: 4-byte length + the data.
    while {$key ne "e"} {
        if {[string first $key "abcd"] < 0} { error "bit2bin: unexpected key '$key' in $bitfile" }
        binary scan $d @${pos}Su len
        set text [string trimright [string range $d [expr {$pos + 2}] [expr {$pos + 1 + $len}]] "\0"]
        puts "bit2bin: $key = $text"
        incr pos [expr {2 + $len}]
        set key [string index $d $pos]
        incr pos
    }
    binary scan $d @${pos}Iu len
    incr pos 4
    set data [string range $d $pos [expr {$pos + $len - 1}]]
    if {[string length $data] != $len || $len % 4 != 0} {
        error "bit2bin: bad data length $len in $bitfile"
    }

    # Swap: read big-endian 32-bit words, write them little-endian.
    binary scan $data Iu* words
    set out [binary format iu* $words]
    if {[string first "\x66\x55\x99\xaa" $out] < 0} {
        error "bit2bin: no sync word found in $bitfile"
    }

    set f [open $binfile wb]
    puts -nonewline $f $out
    close $f
    puts "bit2bin: wrote $binfile ($len bytes)"
}

# Run directly: tclsh bit2bin.tcl in.bit out.bin
if {[info exists argv0] && [file tail $argv0] eq "bit2bin.tcl"} {
    if {[llength $argv] != 2} { puts "usage: tclsh bit2bin.tcl in.bit out.bin"; exit 1 }
    bit2bin [lindex $argv 0] [lindex $argv 1]
}
