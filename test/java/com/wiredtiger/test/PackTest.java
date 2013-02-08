package com.wiredtiger.test;

import com.wiredtiger.db.PackInputStream;
import com.wiredtiger.db.PackOutputStream;
import com.wiredtiger.db.WiredTigerPackingException;

public class PackTest {

	private void printByteArray(byte[] bytes, int len) {
		for (int i = 0; i <= len; i++) {
			System.out.println(String.format(
				"\t%8s", Integer.toBinaryString(
				bytes[i] & 0xff)).replace(' ', '0'));
		}
	}

	static void TestPack01() {
		String format = "b";
		PackInputStream packer = new PackInputStream(format);
		try {
			packer.addFieldByte((byte)8);
		} catch (WiredTigerPackingException wtpe) {
			System.out.println("Error packing: " + wtpe);
		}

		if (!format.equals(packer.getFormat())) {
			System.out.println(
			    "Format string mismatch. Expected: " +
			    format + " got: " + packer.getFormat());
		}
		byte[] packed = packer.getValue();
		System.out.println("Format: " + format);
		System.out.println("Packed: " +
		    Integer.toBinaryString(packed[0] & 0xff));

		PackOutputStream unpacker =
			new PackOutputStream(format, packed);
		try {
			if (unpacker.getFieldByte() != (byte)8)
				System.out.println("unpack mismatch.");
		} catch (WiredTigerPackingException wtpe) {
			System.out.println("Error unpacking: " + wtpe);
		}
	}

	static void TestPack02() {
		String format = "biqrhS";
		PackInputStream packer = new PackInputStream(format);
		try {
			packer.addFieldByte((byte)8);
			packer.addFieldInt(124);
			packer.addFieldLong(1240978);
			packer.addFieldRecord(5680234);
			packer.addFieldShort((short)8576);
			packer.addFieldString("Hello string");
		} catch (WiredTigerPackingException wtpe) {
			System.out.println("Error packing: " + wtpe);
		}

		if (!format.equals(packer.getFormat())) {
			System.out.println(
			    "Format string mismatch. Expected: " +
			    format + " got: " + packer.getFormat());
		}
		byte[] packed = packer.getValue();
		System.out.println("Format: " + format);
		System.out.println("Packed: " + new String(packed));

		PackOutputStream unpacker =
			new PackOutputStream(format, packed);
		try {
			if (unpacker.getFieldByte() != (byte)8)
				System.out.println("unpack mismatch 1.");
			if (unpacker.getFieldInt() != 124)
				System.out.println("unpack mismatch 3.");
			if (unpacker.getFieldLong() != 1240978)
				System.out.println("unpack mismatch 4.");
			if (unpacker.getFieldRecord() != 5680234)
				System.out.println("unpack mismatch 5.");
			if (unpacker.getFieldShort() != 8576)
				System.out.println("unpack mismatch 6.");
			if (!unpacker.getFieldString().equals("Hello string"))
				System.out.println("unpack mismatch 7.");
		} catch (WiredTigerPackingException wtpe) {
			System.out.println("Error unpacking: " + wtpe);
		}
	}

	static void TestUnpack() {
	}

	static void TestPackUnpackNumber() {
		long[] testers = {
			-12,
			-145,
			-14135,
			-1352308572,
			-1,
			0,
			1,
			12,
			145,
			12314,
			873593485,
			-30194371,
			-4578928,
			75452136,
			-28619244,
			93580892,
			83350219,
			27407091,
			-82413912,
			-727169,
			-3748613,
			54046160,
			-49539872,
			-4517158,
			20397230,
			-68522195,
			61663315,
			-6009306,
			-57778143,
			-97631892,
			-62388819,
			23581637,
			2417807,
			-17761744,
			-4174142,
			92685293,
			84597598,
			-83143925,
			95302021,
			90888796,
			88697909,
			-89601258,
			93585507,
			63705051,
			51191732,
			60900034,
			-93016118,
			-68693051,
			-49366599,
			-90203871,
			58404039,
			-79195628,
			-98043222,
			35325799,
			47942822,
			11582824,
			93322027,
			71799760,
			65114434,
			42851643,
			69146495,
			-86855643,
			40073283,
			1956899,
			28090147,
			71274080,
			-95192279,
			-30641467,
			-1142067,
			-32599515,
			92478069,
			-90277478,
			-39370258,
			-77673053,
			82435569,
			88167012,
			-39048877,
			96895962,
			-8587864,
			-70095341,
			49508886,
			69912387,
			24311011,
			-58758419,
			63228782,
			-52050021,
			24687766,
			34342885,
			97830395,
			74658034,
			-9715954,
			-76120311,
			-63117710,
			-19312535,
			42829521,
			32389638,
			-51273506,
			16329653,
			-39061706,
			-9931233,
			42174615,
			75412082,
			-26236331,
			57741055,
			-17577762,
			3605997,
			-73993355,
			-54545904,
			-86580638,
			84432898,
			-83573465,
			-1278,
			636,
			-9935,
			9847,
			8300,
			-5170,
			-2501,
			6031,
			-6658,
			-9780,
			-5351,
			6573,
			-5582,
			-1994,
			-7498,
			-5190,
			7710,
			-8125,
			-6478,
			3670,
			4293,
			1903,
			2367,
			3501,
			841,
			-1718,
			-2303,
			-670,
			9668,
			8391,
			3719,
			1453,
			7203,
			-9693,
			1294,
			-3549,
			-8941,
			-5455,
			30,
			2773,
			8354,
			7272,
			-9794,
			-4806,
			-7091,
			-8404,
			8297,
			-4093,
			-9890,
			-4948,
			-38,
			-66,
			-12,
			9,
			50,
			-26,
			4,
			-25,
			62,
			2,
			47,
			-40,
			-22,
			-87,
			75,
			-43,
			-51,
			65,
			7,
			-17,
			-90,
			-27,
			56,
			-60,
			27
		};
		/* Need to update..
		PackInputStream packer = new PackInputStream();
		PackOutputStream unpacker = new PackOutputStream();
		for (long i:testers) {
			byte[] bytes = new byte[21];
			int len = packer.packLong(bytes, i);
			System.out.println(i);
			printByteArray(bytes, len);
			// Verify that unpack works.
			long unpacked = unpacker.unpackLong(bytes);
			if (i != unpacked)
				System.out.println(
					i + " did not match " + unpacked);
		}
		*/
	}

	public static void main(String[] args) {
		TestPack01();
		TestPack02();
		TestUnpack();
		TestPackUnpackNumber();
	}
}
