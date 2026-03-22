from yeelight import Bulb


def read_the_stuff_i_need(filepath: str) -> dict[str, str]:
    return {
        line.split(" ")[0]: line.split(" ")[1]
        for line in list(
            map(lambda _line: _line[0:-1], open(filepath, "r").readlines())
        )
        if not (line[0] == "_" or line[-1] == "_")
    }


def set_rgba(bulb: Bulb, r: int, g: int, b: int, a: int) -> None:
    bulb.set_rgb(r, g, b)
    bulb.set_brightness(a)


if __name__ == "__main__":
    device_info: dict[str, str] = read_the_stuff_i_need("./PRIVATE.txt")

    bulb_dict: dict[str, Bulb] = {
        name: Bulb(ip_address, auto_on=False)
        for name, ip_address in device_info.items()
    }

    for name, bulb in bulb_dict.items():
        set_rgba(bulb, 0, 0, 255, 1)
