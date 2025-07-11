import typer

from tools.flag_sync import flag, namespace

app = typer.Typer()
app.add_typer(flag.app, name="flag")
app.add_typer(namespace.app, name="namespace")

if __name__ == "__main__":
    app()
