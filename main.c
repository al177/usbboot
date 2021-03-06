#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

int signed_boot = 0;
int verbose = 0;
int loop = 0;
int overlay = 0;
long delay = 500;
char * directory = NULL;
char pathname[18];

int out_ep;
int in_ep;

#define SAME_TIMEOUT_DEFAULT 60000;

typedef struct MESSAGE_S {
		int length;
		unsigned char signature[20];
} boot_message_t;

void usage(int error)
{
	FILE * dest = error ? stderr : stdout;

	fprintf(dest, "Usage: rpiboot\n");
	fprintf(dest, "   or: rpiboot -d [directory]\n");
	fprintf(dest, "Boot a Raspberry Pi in device mode either directly into a mass storage device\n");
	fprintf(dest, "or provide a set of boot files in a directory from which to boot.  This can\n");
	fprintf(dest, "then contain a initramfs to boot through to linux kernel\n\n");
	fprintf(dest, "rpiboot                  : Boot the device into mass storage device\n");
	fprintf(dest, "rpiboot -d [directory]   : Boot the device using the boot files in 'directory'\n");
	fprintf(dest, "Further options:\n");
	fprintf(dest, "        -l               : Loop forever\n");
	fprintf(dest, "        -o               : Use files from overlay subdirectory if they exist (when using a custom directory)\n");
	fprintf(dest, "                           USB Path (1-1.3.2 for example) is shown in verbose mode.\n");
	fprintf(dest, "                           (bootcode.bin is always preloaded from the base directory)\n");
	fprintf(dest, "        -m delay         : Microseconds delay between checking for new devices (default 500)\n");
	fprintf(dest, "        -v               : Verbose\n");
	fprintf(dest, "        -s               : Signed using bootsig.bin\n");
	fprintf(dest, "        -h               : This help\n");

	exit(error ? -1 : 0);
}

libusb_device_handle * LIBUSB_CALL open_device_with_vid_bus_port(
	libusb_context *ctx, uint16_t vendor_id, uint8_t bus, uint8_t port)
{
	struct libusb_device **devs;
	struct libusb_device *found = NULL;
	struct libusb_device *dev;
	struct libusb_device_handle *handle = NULL;
	uint32_t i = 0;
	int r, j, len;
	uint8_t path[8];

	if (libusb_get_device_list(ctx, &devs) < 0)
		return NULL;

	while ((dev = devs[i++]) != NULL) {
		len = 0;
		struct libusb_device_descriptor desc;
		r = libusb_get_device_descriptor(dev, &desc);
		if (r < 0)
			goto out;

		if(overlay || verbose == 2)
		{
			r = libusb_get_port_numbers(dev, path, sizeof(path));
			len = snprintf(&pathname[len], 18-len, "%d", libusb_get_bus_number(dev));
			if (r > 0) {
				len += snprintf(&pathname[len], 18-len, "-");
				len += snprintf(&pathname[len], 18-len, "%d", path[0]);
				for (j = 1; j < r; j++)
				{
					len += snprintf(&pathname[len], 18-len, ".%d", path[j]);
				}
			}
		}

		if(verbose == 2)
		{
			printf("Found device %u idVendor=0x%04x idProduct=0x%04x\n", i, desc.idVendor, desc.idProduct);
			printf("Bus: %d, Device: %d Path: %s\n",libusb_get_bus_number(dev), libusb_get_device_address(dev), pathname);
		}
		// require bus/port match only if bus is !=0
		if ((desc.idVendor == vendor_id) &&
			((bus <= 0) ||
			((libusb_get_bus_number(dev) == bus) &&
			  libusb_get_port_number(dev) == port)))
		{
			if(desc.idProduct == 0x2763 ||
			   desc.idProduct == 0x2764)
			{
				if(verbose) printf("Device located successfully\n");
				found = dev;
				break;
			}
		}
	}

	if (found) {
		sleep(1);
		r = libusb_open(found, &handle);
		if (r < 0)
		{
			if(verbose) printf("Failed to open the requested device\n");
			handle = NULL;
		}
	}

out:
	libusb_free_device_list(devs, 1);
	return handle;

}

#define USB_PORT_FEAT_POWER      (1 << 3)
int cycle_hub_port(libusb_device_handle * usb_device_handle)
{
	int ret,portno;
	libusb_device_handle *hub;
	libusb_device *dev,*hubdev;
	dev=libusb_get_device(usb_device_handle);
	portno=libusb_get_port_number(dev);
	hubdev=libusb_get_parent(dev);
	ret=libusb_open(hubdev,&hub);
	printf("CYCLE (%d): open hub (%d)\n",portno,ret);
	printf("Bus: %d, Device: %d\n",libusb_get_bus_number(hubdev), libusb_get_device_address(hubdev));
	ret = libusb_control_transfer(hub,
	LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
	LIBUSB_REQUEST_CLEAR_FEATURE, USB_PORT_FEAT_POWER, portno, NULL, 0, 10000
	);
	printf("CYCLE (%d): power off port (%d)\n",portno,ret);
	sleep(6);
	ret = libusb_control_transfer(hub,
	LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
	LIBUSB_REQUEST_SET_FEATURE, USB_PORT_FEAT_POWER, portno, NULL, 0, 10000 
	);
	printf("CYCLE (%d): power on port (%d)\n",portno,ret);


	libusb_close(hub);
	return 0;
}

int Initialize_Device(libusb_context ** ctx, libusb_device_handle ** usb_device, uint8_t bus, uint8_t port)
{
	int ret = 0;
	int interface;
	struct libusb_config_descriptor *config;

	*usb_device = open_device_with_vid_bus_port(*ctx, 0x0a5c, bus, port);
	if (*usb_device == NULL)
	{
		usleep(200);
		return -1;
	}

	libusb_get_active_config_descriptor(libusb_get_device(*usb_device), &config);
	if(config == NULL)
	{
		printf("Failed to read config descriptor\n");
		exit(-1);
	}

	// Handle 2837 where it can start with two interfaces, the first is mass storage
	// the second is the vendor interface for programming
	if(config->bNumInterfaces == 1)
	{
		interface = 0;
		out_ep = 1;
		in_ep = 2;
	}
	else
	{
		interface = 1;
		out_ep = 3;
		in_ep = 4;
	}

	ret = libusb_claim_interface(*usb_device, interface);
	if (ret)
	{
		libusb_close(*usb_device);
		printf("Failed to claim interface\n");
		return ret;
	}

	if(verbose) printf("Initialised device correctly\n");

	return ret;
}

int ep_write(void *buf, int len, libusb_device_handle * usb_device)
{
	int a_len = 0;
	int ret =
	    libusb_control_transfer(usb_device, LIBUSB_REQUEST_TYPE_VENDOR, 0,
				    len & 0xffff, len >> 16, NULL, 0, 10000);

	if(ret != 0)
	{
		printf("Failed control transfer (%d,%d)\n", ret, len);
		cycle_hub_port(usb_device);
		return ret;
	}

	if(len > 0)
	{
		ret = libusb_bulk_transfer(usb_device, out_ep, buf, len, &a_len, 100000);
		if(verbose)
			printf("libusb_bulk_transfer returned %d\n", ret);
	}

	return a_len;
}

int ep_read(void *buf, int len, libusb_device_handle * usb_device)
{
	int ret =
	    libusb_control_transfer(usb_device,
				    LIBUSB_REQUEST_TYPE_VENDOR |
				    LIBUSB_ENDPOINT_IN, 0, len & 0xffff,
				    len >> 16, buf, len, 2000);
	if(ret >= 0)
		return len;
	else
		return ret;
}

void get_options(int argc, char *argv[])
{
	// Skip the command name
	argv++; argc--;
	while(*argv)
	{
		if(strcmp(*argv, "-d") == 0)
		{
			argv++; argc--;
			if(argc < 1)
				usage(1);
			directory = *argv;
		}
		else if(strcmp(*argv, "-h") == 0 || strcmp(*argv, "--help") == 0)
		{
			usage(0);
		}
		else if(strcmp(*argv, "-l") == 0)
		{
			loop = 1;
		}
		else if(strcmp(*argv, "-v") == 0)
		{
			verbose = 1;
		}
		else if(strcmp(*argv, "-o") == 0)
		{
			overlay = 1;
		}
		else if(strcmp(*argv, "-m") == 0)
		{
			argv++; argc--;
			if(argc < 1)
				usage(1);
			delay = atol(*argv);
		}
		else if(strcmp(*argv, "-vv") == 0)
		{
			verbose = 2;
		}
		else if(strcmp(*argv, "-s") == 0)
		{
			signed_boot = 1;
		}
		else
		{
			usage(1);
		}

		argv++; argc--;
	}
	if(overlay&&!directory)
	{
		usage(1);
	}
	if(!delay)
	{
		usage(1);
	}
}

boot_message_t boot_message;
void *second_stage_txbuf;

int second_stage_prep(FILE *fp, FILE *fp_sig)
{
	int size;

	fseek(fp, 0, SEEK_END);
	boot_message.length = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if(fp_sig != NULL)
	{
		fread(boot_message.signature, 1, sizeof(boot_message.signature), fp_sig);
	}

	second_stage_txbuf = (uint8_t *) malloc(boot_message.length);
	if (second_stage_txbuf == NULL)
	{
		printf("Failed to allocate memory\n");
		return -1;
	}

	size = fread(second_stage_txbuf, 1, boot_message.length, fp);
	if(size != boot_message.length)
	{
		printf("Failed to read second stage\n");
		return -1;
	}

	return 0;
}

int second_stage_boot(libusb_device_handle *usb_device)
{
	int size, retcode = 0;

	size = ep_write(&boot_message, sizeof(boot_message), usb_device);
	if (size != sizeof(boot_message))
	{
		printf("Failed to write correct length, returned %d\n", size);
		return -1;
	}

	if(verbose) printf("Writing %d bytes\n", boot_message.length);
	size = ep_write(second_stage_txbuf, boot_message.length, usb_device);
	if (size != boot_message.length)
	{
		printf("Failed to read correct length, returned %d\n", size);
		return -1;
	}

	sleep(1);
	size = ep_read((unsigned char *)&retcode, sizeof(retcode), usb_device);

	if (size > 0 && retcode == 0)
	{
		printf("Successful read %d bytes \n", size);
	}
	else
	{
		printf("Failed : 0x%x", retcode);
	}

	return retcode;

}


FILE * check_file(char * dir, char *fname)
{
	FILE * fp = NULL;
	char path[256];

	// Check directory first then /usr/share/rpiboot
	if(dir)
	{
		if(overlay&&(pathname != NULL))
		{
			strcpy(path, dir);
			strcat(path, "/");
			strcat(path, pathname);
			strcat(path, "/");
			strcat(path, fname);
			fp = fopen(path, "rb");
			memset(path, 0, sizeof(path));
		}

		if (fp == NULL)
		{
			strcpy(path, dir);
			strcat(path, "/");
			strcat(path, fname);
			fp = fopen(path, "rb");
		}
	}

	if(fp == NULL)
	{
		strcpy(path, "/usr/share/rpiboot/msd/");
		strcat(path, fname);
		fp = fopen(path, "rb");
	}

	return fp;
}

int file_server(libusb_device_handle * usb_device)
{
	int going = 1;
	struct file_message {
		int command;
		char fname[256];
	} message;
	static FILE * fp = NULL;

	while(going)
	{
		char message_name[][20] = {"GetFileSize", "ReadFile", "Done"};
		int i = ep_read(&message, sizeof(message), usb_device);
		if(i < 0)
		{
			// Drop out if the device goes away
			if(i == LIBUSB_ERROR_NO_DEVICE || i == LIBUSB_ERROR_IO)
				break;
			sleep(1);
			continue;
		}
		if(verbose) printf("Received message %s: %s\n", message_name[message.command], message.fname);

		// Done can also just be null filename
		if(strlen(message.fname) == 0)
		{
			ep_write(NULL, 0, usb_device);
			break;
		}

		switch(message.command)
		{
			case 0: // Get file size
				if(fp)
					fclose(fp);
				fp = check_file(directory, message.fname);
				if(strlen(message.fname) && fp != NULL)
				{
					int file_size;

					fseek(fp, 0, SEEK_END);
					file_size = ftell(fp);
					fseek(fp, 0, SEEK_SET);

					if(verbose) printf("File size = %d bytes\n", file_size);

					int sz = libusb_control_transfer(usb_device, LIBUSB_REQUEST_TYPE_VENDOR, 0,
					    file_size & 0xffff, file_size >> 16, NULL, 0, 1000);

					if(sz < 0)
						return -1;
				}
				else
				{
					ep_write(NULL, 0, usb_device);
					if(verbose) printf("Cannot open file %s\n", message.fname);
					break;
				}
				break;

			case 1: // Read file
				if(fp != NULL)
				{
					int file_size;
					void *buf;

					printf("File read: %s\n", message.fname);

					fseek(fp, 0, SEEK_END);
					file_size = ftell(fp);
					fseek(fp, 0, SEEK_SET);

					buf = malloc(file_size);
					if(buf == NULL)
					{
						printf("Failed to allocate buffer for file %s\n", message.fname);
						return -1;
					}
					int read = fread(buf, 1, file_size, fp);
					if(read != file_size)
					{
						printf("Failed to read from input file\n");
						free(buf);
						return -1;
					}

					int sz = ep_write(buf, file_size, usb_device);

					free(buf);
					fclose(fp);
					fp = NULL;

					if(sz != file_size)
					{
						printf("Failed to write complete file to USB device\n");
						return -1;
					}
				}
				else
				{
					if(verbose) printf("No file %s found\n", message.fname);
					ep_write(NULL, 0, usb_device);
				}
				break;

			case 2: // Done, exit file server
				going = 0;
				break;

			default:
				printf("Unknown message\n");
				return -1;
		}
	}

	printf("Second stage boot server done\n");
	return 0;
}

int main(int argc, char *argv[])
{
	FILE * second_stage;
	FILE * fp_sign = NULL;
	libusb_context *ctx;
	libusb_device_handle *usb_device;
	struct libusb_device_descriptor desc;
	struct libusb_config_descriptor *config;
	uint8_t same_bus = 0;
	uint8_t	same_port;
	int same_timeout = 0;

	get_options(argc, argv);

	// flush immediately
	setbuf(stdout, NULL);

#if defined (__CYGWIN__)
	//printf("Running under Cygwin\n");
#else
	//exit if not run as sudo
	if(getuid() != 0)
	{
		printf("Must be run with sudo...\n");
		exit(-1);
	}
#endif


	// Default to standard msd directory
	if(directory == NULL)
		directory = "msd";

	second_stage = check_file(directory, "bootcode.bin");
	if(second_stage == NULL)
	{
		fprintf(stderr, "Unable to open 'bootcode.bin' from /usr/share/rpiboot/msd or supplied directory\n");
		usage(1);
	}

	if(signed_boot)
	{
		fp_sign = check_file(directory, "bootsig.bin");
		if(fp_sign == NULL)
		{
			fprintf(stderr, "Unable to open 'bootsig.bin'\n");
			usage(1);
		}
	}

	if(second_stage_prep(second_stage, fp_sign) != 0)
	{
		fprintf(stderr, "Failed to prepare the second stage bootcode\n");
		exit(-1);
	}

	int ret = libusb_init(&ctx);
	if (ret)
	{
		printf("Failed to initialise libUSB\n");
		exit(-1);
	}

	libusb_set_debug(ctx, verbose ? LIBUSB_LOG_LEVEL_WARNING : 0);

	do
	{
		int last_serial = -1;

		printf("Waiting for BCM2835/6/7\n");

		// Wait for a device to get plugged in
		do
		{
			// Timeout check for same bus/port filter
			if(same_bus != 0)
			{
				if (same_timeout <= 0)
				{
					// timeout exceeded, clear the same bus/port filter
					same_bus = 0;
					same_port = 0;
					if(verbose) printf("Bus/port timeout exceeded, clearing bus check flag\n");
				}
				else
				{
					same_timeout--;
				}
			}
			ret = Initialize_Device(&ctx, &usb_device, same_bus, same_port);
			if(ret == 0)
			{
				libusb_get_device_descriptor(libusb_get_device(usb_device), &desc);

				if(verbose)
					printf("Found serial number %d\n", desc.iSerialNumber);

				// Make sure we've re-enumerated since the last time
				if(desc.iSerialNumber == last_serial)
				{
					ret = -1;
					libusb_close(usb_device);
				}

				libusb_get_active_config_descriptor(libusb_get_device(usb_device), &config);
			}

			if (ret)
			{
				usleep(delay);
			}
		}
		while (ret);

		last_serial = desc.iSerialNumber;
		if(desc.iSerialNumber == 0)
		{
			printf("Sending bootcode.bin\n");
			same_bus = libusb_get_bus_number(libusb_get_device(usb_device));
			same_port = libusb_get_port_number(libusb_get_device(usb_device));
			same_timeout = SAME_TIMEOUT_DEFAULT;
			if(verbose) printf(" set same_bus= %d same_port= %d same timeout %d\n", same_bus, same_port, same_timeout);
			if (second_stage_boot(usb_device))
			{
				// Failed, stop checking for the same bus/port
				same_bus=0;
				same_port=0;
			}
		}
		else if(same_bus)
		{
			printf("Second stage boot server\n");
			file_server(usb_device);
			// Done with an upload, stop checking for the same bus/port
			same_bus=0;
			same_port=0;
		}


		libusb_close(usb_device);
		sleep(1);
	}
	while(loop || desc.iSerialNumber == 0);

	libusb_exit(ctx);

	return 0;
}

