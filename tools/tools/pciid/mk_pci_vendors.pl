#!/usr/bin/perl -w
#
# Copyright (C) 2001 Sheldon Hearn.  All rights reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
# 
# $FreeBSD$
#
# usage: mk_pci_vendors [-lq] [-p pcidevs.txt] [-v vendors.txt]
#
# Generate src/share/misc/pci_vendors from the Hart and Boemler lists,
# currently available at:
#
# Boemler:	http://www.yourvote.com/pci/
# Hart:		http://members.datafast.net.au/dft0802/downloads.htm
#
# -l	Where an entry is found in both input lists, use the entry with
#	the longest description.  The default is for the Boemler file to
#	override the Hart file.
# -q	Do not print diagnostics.
# -p	Specify the pathname of the Hart file. (Default ./pcidevs.txt)
# -v	Specify the pathname of the Boemler file. (Default ./vendors.txt)
#
use strict;
use Getopt::Std;

my $PROGNAME = 'mk_pci_vendors';
my $VENDORS_FILE = 'vendors.txt';
my $PCIDEVS_FILE = 'pcidevs.txt';

my $cur_vendor;
my %opts;
my %vendors;
my ($descr, $existing, $id, $line, $rv, $winner, $optlused);

my $IS_VENDOR = 1;
my $IS_DEVICE = 2;
my $V_DESCR = 0;
my $V_DEVSL = 1;
my $W_NOCONTEST = 0;
my $W_VENDORS = 1;
my $W_PCIDEVS = 2;

sub clean_descr($);
sub vendors_parse($\$\$);
sub pcidevs_parse($\$\$);

if (not getopts('lp:qv:', \%opts) or @ARGV > 0) {
	print STDERR "usage: $PROGNAME [-lq] [-p pcidevs.txt] [-v vendors.txt]\n";
	exit 1;
}

if (not defined($opts{p})) {
	$opts{p} = $PCIDEVS_FILE;
}
if (not defined($opts{v})) {
	$opts{v} = $VENDORS_FILE;
}
foreach (('l', 'q')) {
	if (not exists($opts{$_})) {
		$opts{$_} = 0;
	} else {
		$opts{$_} = 1;
	}
}

open(VENDORS, "< $opts{v}") or
    die "$PROGNAME: $opts{v}: $!\n";
while ($line = <VENDORS>) {
	chomp($line);
	$rv = vendors_parse($line, $id, $descr);
	if ($rv == $IS_VENDOR) {
		if (exists($vendors{$id})) {
			die "$PROGNAME: $id: duplicate vendor ID\n";
		}
		$vendors{$id} = [$descr, {}];
		$cur_vendor = $id;
	} elsif ($rv == $IS_DEVICE) {
		${$vendors{$cur_vendor}->[$V_DEVSL]}{$id} = $descr;
	}
}
close(VENDORS);

open(PCIDEVS, "< $opts{p}") or
    die "$PROGNAME: $opts{p}: $!\n";
while ($line = <PCIDEVS>) {
	chomp($line);
	$rv = pcidevs_parse($line, $id, $descr);
	if ($rv == $IS_VENDOR) {
		if (not exists($vendors{$id})) {
			$vendors{$id} = [$descr, {}];
			$winner = $W_NOCONTEST;
		} elsif ($opts{l}) {
			$existing = $vendors{$id}->[$V_DESCR];
			if (length($existing) < length($descr)) {
				$vendors{$id}->[$V_DESCR] = $descr;
				$winner = $W_PCIDEVS;
			} else {
				$winner = $W_VENDORS;
			}
		} else {
			$winner = $W_VENDORS;
		}
		$cur_vendor = $id;
		if (not $opts{q} and $winner != $W_NOCONTEST) {
			$existing = $vendors{$id}->[$V_DESCR];
			print STDERR "$PROGNAME: ",
			    $winner == $W_VENDORS ? "Boemler" : "Hart",
			    " vendor wins: $id\t$existing\n";
		}
	} elsif ($rv == $IS_DEVICE) {
		if (not exists(${$vendors{$cur_vendor}->[$V_DEVSL]}{$id})) {
			${$vendors{$cur_vendor}->[$V_DEVSL]}{$id} = $descr;
			$winner = $W_NOCONTEST;
		} elsif ($opts{l}) {
			$existing = ${$vendors{$cur_vendor}->[$V_DEVSL]}{$id};
			if (length($existing) < length($descr)) {
				${$vendors{$cur_vendor}->[$V_DEVSL]}{$id} =
				    $descr;
				$winner = $W_PCIDEVS;
			} else {
				$winner = $W_VENDORS;
			}
		} else {
			$winner = $W_VENDORS;
		}
		if (not $opts{q} and $winner != $W_NOCONTEST) {
			$existing = ${$vendors{$cur_vendor}->[$V_DEVSL]}{$id};
			print STDERR "$PROGNAME: ",
			    $winner == $W_VENDORS ? "Boemler" : "Hart",
			    " device wins: $id\t$existing\n";
		}
	}
}
close(PCIDEVS);

$optlused = $opts{l} ? "with" : "without";
print <<HEADER_END;
; \$FreeBSD\$
;
; Automatically generated by src/tools/tools/pciid/mk_pci_vendors.pl
; ($optlused the -l option), using the following source lists:
;
;	http://www.yourvote.com/pci/vendors.txt
;	http://members.hyperlink.com.au/~chart/download/pcidevs.txt
;
; Manual edits on this file will be lost!
;
HEADER_END

foreach $cur_vendor (sort keys %vendors) {
	$id = $cur_vendor;
	$descr = $vendors{$id}->[$V_DESCR];
	print "$id\t$descr\n";
	foreach $id (sort keys %{$vendors{$cur_vendor}->[$V_DEVSL]}) {
		$descr = ${$vendors{$cur_vendor}->[$V_DEVSL]}{$id};
		print "\t$id\t$descr\n";
	}
}
exit 0;


# Parse a line from the Boemler file and place the ID and description
# in the scalars referenced by $id_ref and $descr_ref.
#
# On success, returns $IS_VENDOR if the line represents a vendor entity
# or $IS_DEVICE if the line represents a device entity.
#
# Returns 0 on failure.
#
sub vendors_parse($\$\$)
{
	my ($line, $id_ref, $descr_ref) = @_;

	if ($line =~ /^([A-Fa-f0-9]{4})\t([^\t].+?)\s*$/) {
		($$id_ref, $$descr_ref) = (uc($1), clean_descr($2));
		return $IS_VENDOR;
	} elsif ($line =~ /^\t([A-Fa-f0-9]{4})\t([^\t].+?)\s*$/) {
		($$id_ref, $$descr_ref) = (uc($1), clean_descr($2));
		return $IS_DEVICE;
	} elsif (not $opts{q} and
	    $line !~ /^\s*$/ and $line !~ /^;/) {
		chomp($line);
		print STDERR "$PROGNAME: ignored Boemler: $line\n";
	}

	return 0;
}

# Parse a line from the Hart file and place the ID and description
# in the scalars referenced by $id_ref and $descr_ref.
#
# On success, returns $IS_VENDOR if the line represents a vendor entity
# or $IS_DEVICE if the line represents a device entity.
#
# Returns 0 on failure.
#
sub pcidevs_parse($\$\$)
{
	my ($line, $id_ref, $descr_ref) = @_;
	my $descr;

	if ($line =~ /^V\t([A-Fa-f0-9]{4})\t([^\t].+?)\s*$/) {
		($$id_ref, $$descr_ref) = (uc($1), clean_descr($2));
		return $IS_VENDOR;
	} elsif ($line =~ /^D\t([A-Fa-f0-9]{4})\t([^\t].+?)\s*$/) {
		($$id_ref, $$descr_ref) = (uc($1), clean_descr($2));
		return $IS_DEVICE;
	} elsif (not $opts{q} and
	    $line !~ /^\s*$/ and $line !~ /^[;ORSX]/) {
		print STDERR "$PROGNAME: ignored Hart: $line\n";
	}

	return 0;
}

sub clean_descr($)
{
	my ($descr) = @_;

	return $descr;
}
