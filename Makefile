lint:
	black .
	mypy .

all: lint